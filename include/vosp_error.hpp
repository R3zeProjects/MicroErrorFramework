#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace vosp::error
{
    /**
     * @brief Categories used to route errors to their corresponding registers.
     */
    enum class Category : std::uint16_t
    {
        NETWORK = 0,
        DATABASE,
        FILESYSTEM,
        NONE = 0xffff
    };

    /**
     * @brief Error value containing a code, message and optional category.
     */
    class Error
    {
    public:
        /**
         * @brief Creates an error value.
         * @param category Error category used for routing.
         * @param code Stable application-specific error code.
         * @param message Human-readable error message.
         */
        explicit Error(Category category,
                       std::uint32_t code,
                       std::string message)
            : message_(std::move(message)), code_(code), category_(category)
        {
        }

        /** @brief Compares all owned error fields; C++ also synthesizes operator!=. */
        [[nodiscard]] bool operator==(const Error&) const noexcept = default;

        /** @brief Returns the stable application-specific error code. */
        [[nodiscard]] std::uint32_t code() const noexcept
        {
            return code_;
        }

        /** @brief Returns the human-readable error message. */
        [[nodiscard]] std::string_view message() const noexcept
        {
            return message_;
        }

        /** @brief Returns the error category. */
        [[nodiscard]] Category category() const noexcept
        {
            return category_;
        }

        /** @brief Returns true when the error has a category other than NONE. */
        [[nodiscard]] bool has_category() const noexcept
        {
            return category_ != Category::NONE;
        }

    private:
        std::string message_;
        std::uint32_t code_;
        Category category_;
    };

    /**
     * @brief Hash function for using Error in unordered containers.
     */
    struct ErrorHash
    {
        [[nodiscard]] std::size_t operator()(const Error& error) const noexcept
        {
            const auto code_hash = std::hash<std::uint32_t>{}(error.code());
            const auto message_hash = std::hash<std::string_view>{}(error.message());
            const auto category_hash = std::hash<std::uint16_t>{}(
                static_cast<std::uint16_t>(error.category()));

            return code_hash ^ (message_hash + 0x9e3779b9u +
                                (code_hash << 6u) + (code_hash >> 2u)) ^
                   (category_hash << 1u);
        }
    };

    /**
     * @brief Result type for operations that return either a value or an Error.
     * @tparam T Value type returned by a successful operation.
     */
    template<typename T>
    using Result = std::expected<T, Error>;

    using OperationResult = Result<void>;

    inline constexpr std::uint32_t duplicate_error_code = 0xE001;
    inline constexpr std::uint32_t missing_error_code = 0xE002;
    inline constexpr std::uint32_t missing_register_code = 0xE003;
    inline constexpr std::uint32_t register_capacity_error_code = 0xE005;
    inline constexpr std::uint32_t register_category_error_code = 0xE006;
    inline constexpr std::size_t max_register_capacity = 1'000'000;

    namespace predefined
    {
        [[nodiscard]] inline Error make(
            Category category,
            std::uint32_t code,
            std::string_view message) noexcept
        {
            try
            {
                return Error{category, code, std::string{message}};
            }
            catch (...)
            {
                return Error{category, code, {}};
            }
        }

        [[maybe_unused]] inline const Error network_error =
            make(Category::NETWORK, 1000, "Network error");
        [[maybe_unused]] inline const Error database_error =
            make(Category::DATABASE, 2000, "Database error");
        [[maybe_unused]] inline const Error filesystem_error =
            make(Category::FILESYSTEM, 3000, "Filesystem error");
        [[maybe_unused]] inline const Error uncategorized_error =
            make(Category::NONE, 0, "Uncategorized error");
    }


    /**
     * @brief Interface for a category-specific error register.
     * @note A register must outlive every handler or system that references it.
     */
    class IRegister
    {
    public:
        /**
         * @brief Registers an error in this category-specific storage.
         * @param error Error to register.
         * @return Empty result on success, or an Error describing the failure.
         */
        [[nodiscard]] virtual OperationResult add(const Error& error) = 0;

        /**
         * @brief Removes an error from this category-specific storage.
         * @param error Error to remove.
         * @return Empty result on success, or an Error describing the failure.
         */
        [[nodiscard]] virtual OperationResult remove(const Error& error) = 0;

        /** @brief Returns the category handled by this register. */
        [[nodiscard]] virtual Category category() const noexcept = 0;

        virtual ~IRegister() noexcept = default;
    };

    /**
     * @brief Base class for a register dedicated to one category.
     * @tparam RegisterCategory Category handled by the register.
     */
    template<Category RegisterCategory>
    class CategoryRegister : public IRegister
    {
    public:
        [[nodiscard]] Category category() const noexcept final
        {
            return RegisterCategory;
        }

        ~CategoryRegister() noexcept override = default;
    };

    /**
     * @brief Thread-safe in-memory register for one error category.
     * @tparam RegisterCategory Category handled by the register.
     */
    template<Category RegisterCategory>
    class MemoryRegister final : public CategoryRegister<RegisterCategory>
    {
    public:
        /**
         * @brief Creates a bounded register and reserves its initial storage.
         * @param expected_size Initial number of elements to reserve.
         * @param capacity_limit Maximum number of errors retained by this instance.
         * @throws std::invalid_argument If either limit is invalid.
         */
        explicit MemoryRegister(
            std::size_t expected_size = 64,
            std::size_t capacity_limit = max_register_capacity)
            : capacity_limit_{capacity_limit}
        {
            validate_limits(expected_size, capacity_limit_);
            errors_.reserve(expected_size);
        }

        /**
         * @brief Reserves storage for at least the requested number of errors.
         * @throws std::invalid_argument If the request exceeds the capacity limit.
         */
        void reserve(std::size_t expected_size)
        {
            validate_limits(expected_size, capacity_limit_);
            const std::lock_guard lock{mutex_};
            errors_.reserve(expected_size);
        }

        /** @brief Returns the configured maximum number of stored errors. */
        [[nodiscard]] std::size_t capacity_limit() const noexcept
        {
            return capacity_limit_;
        }

        /** @copydoc IRegister::add */
        [[nodiscard]] OperationResult add(const Error& error) override
        {
            const std::lock_guard lock{mutex_};

            if (error.category() != RegisterCategory)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    register_category_error_code,
                    "Error category does not match this register"
                });
            }

            if (errors_.size() >= capacity_limit_)
            {
                if (errors_.contains(error))
                {
                    return std::unexpected(Error{
                        RegisterCategory,
                        duplicate_error_code,
                        "Error is already registered"
                    });
                }

                return std::unexpected(Error{
                    RegisterCategory,
                    register_capacity_error_code,
                    "Error register capacity limit reached"
                });
            }

            if (!errors_.insert(error).second)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    duplicate_error_code,
                    "Error is already registered"
                });
            }

            return {};
        }

        /** @copydoc IRegister::remove */
        [[nodiscard]] OperationResult remove(const Error& error) override
        {
            const std::lock_guard lock{mutex_};

            if (error.category() != RegisterCategory)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    register_category_error_code,
                    "Error category does not match this register"
                });
            }

            if (errors_.erase(error) == 0)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    missing_error_code,
                    "Error is not registered"
                });
            }

            return {};
        }

        /** @brief Returns whether an equal error is currently registered. */
        [[nodiscard]] bool contains(const Error& error) const
        {
            const std::lock_guard lock{mutex_};
            return errors_.contains(error);
        }

        /** @brief Returns the current number of registered errors. */
        [[nodiscard]] std::size_t size() const noexcept
        {
            const std::lock_guard lock{mutex_};
            return errors_.size();
        }

    private:
        static void validate_limits(std::size_t expected_size, std::size_t capacity_limit)
        {
            if (capacity_limit == 0 || capacity_limit > max_register_capacity)
            {
                throw std::invalid_argument(
                    "MemoryRegister capacity must be in [1, max_register_capacity]");
            }

            if (expected_size > capacity_limit)
            {
                throw std::invalid_argument(
                    "MemoryRegister expected size exceeds its capacity limit");
            }
        }

        std::size_t capacity_limit_ = max_register_capacity;
        mutable std::mutex mutex_;
        std::unordered_set<Error, ErrorHash> errors_;
    };

    /**
     * @brief Restricts a type to implementations of IRegister.
     * @tparam Register Candidate register type.
     */
    template<typename Register>
    concept RegisterType = std::derived_from<std::remove_cvref_t<Register>, IRegister>;

    namespace detail
    {
        template<bool Synchronized, RegisterType... Registers>
        class BasicHandler
        {
        public:
            explicit BasicHandler(Registers&... registers) noexcept
                : registers_{std::ref(registers)...}
            {
            }

            [[nodiscard]] OperationResult add(const Error& error)
            {
                return dispatch(error, &IRegister::add);
            }

            [[nodiscard]] OperationResult remove(const Error& error)
            {
                return dispatch(error, &IRegister::remove);
            }

        private:
            using Operation = OperationResult (IRegister::*)(const Error&);

            [[nodiscard]] OperationResult dispatch(const Error& error, Operation operation)
            {
                for (std::size_t index = 0; index < registers_.size(); ++index)
                {
                    IRegister& register_instance = registers_[index].get();
                    if (register_instance.category() != error.category())
                    {
                        continue;
                    }

                    if constexpr (Synchronized)
                    {
                        const std::lock_guard lock{mutexes_[index]};
                        return std::invoke(operation, register_instance, error);
                    }
                    else
                    {
                        return std::invoke(operation, register_instance, error);
                    }
                }

                return std::unexpected(Error{
                    error.category(),
                    missing_register_code,
                    "No register is configured for this category"
                });
            }

            std::array<std::reference_wrapper<IRegister>, sizeof...(Registers)> registers_;
            std::array<std::mutex, Synchronized ? sizeof...(Registers) : 0> mutexes_;
        };
    }

    /** @brief Routes an error to the first register with a matching category. */
    template<RegisterType... Registers>
    class Handler final : public detail::BasicHandler<false, Registers...>
    {
        using Base = detail::BasicHandler<false, Registers...>;

    public:
        explicit Handler(Registers&... registers) noexcept : Base(registers...) {}
    };

    template<RegisterType... Registers>
    Handler(Registers&...) -> Handler<Registers...>;

    /** @brief Routes errors with one independent lock per category register. */
    template<RegisterType... Registers>
    class ConcurrentHandler final : public detail::BasicHandler<true, Registers...>
    {
        using Base = detail::BasicHandler<true, Registers...>;

    public:
        explicit ConcurrentHandler(Registers&... registers) noexcept : Base(registers...) {}
    };

    template<RegisterType... Registers>
    ConcurrentHandler(Registers&...) -> ConcurrentHandler<Registers...>;

    /**
     * @brief Selects a register implementation without synchronization.
     * @warning The caller must guarantee single-threaded access.
     */
    struct SingleThreadedRegister
    {
    };

    /**
     * @brief Selects a register implementation protected by ErrorSystem's mutex.
     */
    struct MultiThreadedRegister
    {
    };

    /**
     * @brief Asynchronous register policy bound to an external executor.
     * @tparam Executor Executor type satisfying AsyncExecutor.
     */
    template<typename Executor>
    struct AsyncRegister
    {
    };

    /**
     * @brief Selects synchronous dispatch without synchronization.
     */
    struct SingleThreadedHandler
    {
    };

    /**
     * @brief Selects synchronous dispatch protected by ErrorSystem's mutex.
     */
    struct MultiThreadedHandler
    {
    };

    /**
     * @brief Asynchronous handler policy bound to an external executor.
     * @tparam Executor Executor type satisfying AsyncExecutor.
     */
    template<typename Executor>
    struct AsyncHandler
    {
    };

    /**
     * @brief Executor contract used by asynchronous ErrorSystem.
     *
     * submit() must schedule an OperationResult-returning job and return its future.
     */
    template<typename Executor>
    concept AsyncExecutor = requires(Executor& executor, std::function<OperationResult()> job)
    {
        { executor.submit(std::move(job)) } -> std::same_as<std::future<OperationResult>>;
    };

    /**
     * @brief Selects an ErrorSystem specialization by register and handler policies.
     * @tparam TypeRegister Register execution policy.
     * @tparam TypeHandler Handler execution policy.
     * @tparam Registers Non-owning concrete register types.
     */
    template<typename TypeRegister, typename TypeHandler, RegisterType... Registers>
    class ErrorSystem;

    namespace detail
    {
        template<typename HandlerType, RegisterType... Registers>
        class SynchronousErrorSystem
        {
        public:
            explicit SynchronousErrorSystem(Registers&... registers) noexcept
                : handler_(registers...)
            {
            }

            [[nodiscard]] OperationResult add(const Error& error)
            {
                return handler_.add(error);
            }

            [[nodiscard]] OperationResult remove(const Error& error)
            {
                return handler_.remove(error);
            }

        private:
            HandlerType handler_;
        };
    }

    /** @brief Synchronous error system without internal locking. */
    template<RegisterType... Registers>
    class ErrorSystem<SingleThreadedRegister, SingleThreadedHandler, Registers...> final
        : public detail::SynchronousErrorSystem<Handler<Registers...>, Registers...>
    {
        using Base = detail::SynchronousErrorSystem<Handler<Registers...>, Registers...>;

    public:
        using Base::Base;
    };

    /** @brief Synchronous error system with one independent lock per register. */
    template<RegisterType... Registers>
    class ErrorSystem<MultiThreadedRegister, MultiThreadedHandler, Registers...> final
        : public detail::SynchronousErrorSystem<ConcurrentHandler<Registers...>, Registers...>
    {
        using Base = detail::SynchronousErrorSystem<ConcurrentHandler<Registers...>, Registers...>;

    public:
        using Base::Base;
    };

    /**
     * @brief Asynchronous error system using a caller-provided executor.
     * @tparam Executor Executor satisfying AsyncExecutor.
     */
    template<AsyncExecutor Executor, RegisterType... Registers>
    class ErrorSystem<AsyncRegister<Executor>, AsyncHandler<Executor>, Registers...>
    {
    public:
        /**
         * @brief Creates an asynchronous system using an external executor.
         * @param executor Executor that must outlive all submitted operations.
         * @param registers Registers that must outlive all submitted operations.
         */
        explicit ErrorSystem(Executor& executor, Registers&... registers)
            : executor_(executor),
              handler_(std::make_shared<ConcurrentHandler<Registers...>>(registers...))
        {
        }

        /**
         * @brief Schedules an asynchronous add operation.
         * @param error Error copied into the asynchronous task.
         * @return Future containing the operation result.
         */
        [[nodiscard]] std::future<OperationResult> add(Error error)
        {
            return submit(std::move(error), false);
        }

        /**
         * @brief Schedules an asynchronous remove operation.
         * @param error Error copied into the asynchronous task.
         * @return Future containing the operation result.
         */
        [[nodiscard]] std::future<OperationResult> remove(Error error)
        {
            return submit(std::move(error), true);
        }

    private:
        [[nodiscard]] std::future<OperationResult> submit(Error error, bool remove_error)
        {
            const auto handler = handler_;
            return executor_.submit(
                [handler, error = std::move(error), remove_error]() mutable
                {
                    return remove_error ? handler->remove(error) : handler->add(error);
                });
        }

        Executor& executor_;
        std::shared_ptr<ConcurrentHandler<Registers...>> handler_;
    };

    /**
     * @brief Convenient single-threaded ErrorSystem alias.
     * @tparam Registers Concrete register implementations.
     */
    template<RegisterType... Registers>
    using SingleThreadedSystem =
        ErrorSystem<SingleThreadedRegister, SingleThreadedHandler, Registers...>;

    /**
     * @brief Convenient multi-threaded ErrorSystem alias.
     * @tparam Registers Concrete register implementations.
     */
    template<RegisterType... Registers>
    using MultiThreadedSystem =
        ErrorSystem<MultiThreadedRegister, MultiThreadedHandler, Registers...>;

    /**
     * @brief Convenient asynchronous ErrorSystem alias.
     * @tparam Executor External executor implementation.
     * @tparam Registers Concrete register implementations.
     */
    template<AsyncExecutor Executor, RegisterType... Registers>
    using AsyncSystem =
        ErrorSystem<AsyncRegister<Executor>, AsyncHandler<Executor>, Registers...>;

}
