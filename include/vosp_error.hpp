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

        /** @brief Returns true when both error values contain the same data. */
        [[nodiscard]] bool operator==(const Error& rhs) const noexcept
        {
            return code_ == rhs.code_ && message_ == rhs.message_ && category_ == rhs.category_;
        }

        /** @brief Returns true when the error values differ. */
        [[nodiscard]] bool operator!=(const Error& rhs) const noexcept
        {
            return !(*this == rhs);
        }

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
        /** @brief Builds a predefined error without failing static initialization. */
        [[nodiscard]] inline Error make_safe(
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
                // An empty string is normally SSO-backed and avoids another
                // potentially failing allocation during startup recovery.
                return Error{category, code, std::string{}};
            }
        }

        inline const Error network_error = make_safe(Category::NETWORK, 1000, "Network error");
        inline const Error database_error = make_safe(Category::DATABASE, 2000, "Database error");
        inline const Error filesystem_error = make_safe(Category::FILESYSTEM, 3000, "Filesystem error");
        inline const Error uncategorized_error = make_safe(Category::NONE, 0, "Uncategorized error");
    }


    /**
     * @brief Non-owning interface for a category-specific error register.
     * @note Implementations must outlive every Handler that references them.
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
        explicit MemoryRegister(
            std::size_t expected_size = 64,
            std::size_t capacity_limit = max_register_capacity)
            : capacity_limit_{capacity_limit}
        {
            validate_limits(expected_size, capacity_limit_);
            errors_.reserve(expected_size);
        }

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

            const auto it = errors_.find(error);

            if (it == errors_.end())
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    missing_error_code,
                    "Error is not registered"
                });
            }

            errors_.erase(it);
            return {};
        }

        [[nodiscard]] bool contains(const Error& error) const
        {
            const std::lock_guard lock{mutex_};
            return errors_.contains(error);
        }

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

    /**
     * @brief Routes an error to the first register with a matching category.
     * @tparam Registers Concrete register implementations.
     * @note The handler does not own the supplied register objects.
     */
    template<RegisterType... Registers>
    class Handler
    {
    public:
        /**
         * @brief Creates a non-owning dispatcher over the supplied registers.
         * @param registers Register instances that must outlive this handler.
         */
        explicit Handler(Registers&... registers) noexcept
            : registers_{std::ref(registers)...}
        {
        }

        /**
         * @brief Adds an error to the register matching its category.
         * @param error Error to add.
         * @return Empty result on success, or an Error describing the failure.
         */
        [[nodiscard]] OperationResult add(const Error& error)
        {
            return dispatch(error, false);
        }

        /**
         * @brief Removes an error from the register matching its category.
         * @param error Error to remove.
         * @return Empty result on success, or an Error describing the failure.
         */
        [[nodiscard]] OperationResult remove(const Error& error)
        {
            return dispatch(error, true);
        }

    private:
        [[nodiscard]] OperationResult dispatch(const Error& error, bool remove_error)
        {
            for (const auto& register_reference : registers_)
            {
                IRegister& register_instance = register_reference.get();

                if (register_instance.category() != error.category())
                {
                    continue;
                }

                if (remove_error)
                {
                    return register_instance.remove(error);
                }

                return register_instance.add(error);
            }

            return std::unexpected(Error{
                error.category(),
                missing_register_code,
                "No register is configured for this category"
            });
        }

        std::array<std::reference_wrapper<IRegister>, sizeof...(Registers)> registers_;
    };

    /**
     * @brief Category-level synchronized dispatcher.
     *
     * Each register has an independent mutex, so operations targeting
     * different categories can proceed concurrently.
     */
    template<RegisterType... Registers>
    class ConcurrentHandler
    {
    public:
        explicit ConcurrentHandler(Registers&... registers) noexcept
            : registers_{std::ref(registers)...}
        {
        }

        [[nodiscard]] OperationResult add(const Error& error)
        {
            return dispatch(error, false);
        }

        [[nodiscard]] OperationResult remove(const Error& error)
        {
            return dispatch(error, true);
        }

    private:
        [[nodiscard]] OperationResult dispatch(const Error& error, bool remove_error)
        {
            for (std::size_t index = 0; index < registers_.size(); ++index)
            {
                IRegister& register_instance = registers_[index].get();

                if (register_instance.category() != error.category())
                {
                    continue;
                }

                const std::lock_guard lock{mutexes_[index]};
                if (remove_error)
                {
                    return register_instance.remove(error);
                }

                return register_instance.add(error);
            }

            return std::unexpected(Error{
                error.category(),
                missing_register_code,
                "No register is configured for this category"
            });
        }

        std::array<std::reference_wrapper<IRegister>, sizeof...(Registers)> registers_;
        std::array<std::mutex, sizeof...(Registers)> mutexes_;
    };

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

    /**
     * @brief Synchronous error system without internal locking.
     */
    template<RegisterType... Registers>
    class ErrorSystem<SingleThreadedRegister, SingleThreadedHandler, Registers...>
    {
    public:
        /**
         * @brief Creates a single-threaded system over existing registers.
         * @param registers Registers that must outlive this system.
         */
        explicit ErrorSystem(Registers&... registers) noexcept
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
        Handler<Registers...> handler_;
    };

    /**
     * @brief Synchronous error system protected by one mutex.
     */
    template<RegisterType... Registers>
    class ErrorSystem<MultiThreadedRegister, MultiThreadedHandler, Registers...>
    {
    public:
        /**
         * @brief Creates a mutex-protected system over existing registers.
         * @param registers Registers that must outlive this system.
         */
        explicit ErrorSystem(Registers&... registers) noexcept
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
        ConcurrentHandler<Registers...> handler_;
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
         * @param executor Executor that must outlive this system.
         * @param registers Registers that must outlive this system.
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
