#pragma once

#include <vosp/contracts/error.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vosp::error
{
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

    namespace register_policy
    {
        /** @brief Selects internal mutex protection for a register. */
        struct ThreadSafe
        {
            using Mutex = std::mutex;
        };

        /** @brief Omits locking when all access is externally serialized. */
        struct SingleThreaded
        {
            struct Mutex
            {
                constexpr void lock() const noexcept {}
                constexpr void unlock() const noexcept {}
            };
        };
    }

    /** @brief Restricts a type to a register synchronization policy. */
    template<typename Policy>
    concept RegisterPolicy = requires
    {
        typename Policy::Mutex;
    };

    /**
     * @brief Bounded in-memory register for one error category.
     * @tparam RegisterCategory Category handled by the register.
     * @tparam Policy Internal synchronization policy.
     */
    template<Category RegisterCategory,
             RegisterPolicy Policy = register_policy::ThreadSafe>
    class Register final : public CategoryRegister<RegisterCategory>
    {
    public:
        /**
         * @brief Creates a bounded register and reserves its initial storage.
         * @param expected_size Initial number of elements to reserve.
         * @param capacity_limit Maximum number of errors retained by this instance.
         * @throws std::invalid_argument If either limit is invalid.
         */
        explicit Register(
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
                if (errors_.contains(error.code()))
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

            if (!errors_.try_emplace(error.code(), error).second)
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

            const auto stored = errors_.find(error.code());
            if (stored == errors_.end() || stored->second != error)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    missing_error_code,
                    "Error is not registered"
                });
            }

            errors_.erase(stored);

            return {};
        }

        /** @brief Removes an error by its category-local code. */
        [[nodiscard]] OperationResult remove(std::uint32_t code)
        {
            const std::lock_guard lock{mutex_};
            if (errors_.erase(code) == 0)
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    missing_error_code,
                    "Error code is not registered"
                });
            }
            return {};
        }

        /** @brief Returns whether an equal error is currently registered. */
        [[nodiscard]] bool contains(const Error& error) const
        {
            const std::lock_guard lock{mutex_};
            const auto stored = errors_.find(error.code());
            return stored != errors_.end() && stored->second == error;
        }

        /** @brief Returns whether this register contains an error code. */
        [[nodiscard]] bool contains(std::uint32_t code) const
        {
            const std::lock_guard lock{mutex_};
            return errors_.contains(code);
        }

        /**
         * @brief Finds an error by code and returns an owning copy.
         * @note Returning a value keeps the result valid after concurrent mutation.
         */
        [[nodiscard]] std::optional<Error> find(std::uint32_t code) const
        {
            const std::lock_guard lock{mutex_};
            const auto stored = errors_.find(code);
            if (stored == errors_.end())
            {
                return std::nullopt;
            }
            return stored->second;
        }

        /** @brief Returns an owning, concurrency-safe snapshot of all errors. */
        [[nodiscard]] std::vector<Error> snapshot() const
        {
            const std::lock_guard lock{mutex_};
            std::vector<Error> result;
            result.reserve(errors_.size());
            for (const auto& entry : errors_)
            {
                result.push_back(entry.second);
            }
            return result;
        }

        /** @brief Removes every error and returns the previous size. */
        [[nodiscard]] std::size_t clear()
        {
            const std::lock_guard lock{mutex_};
            const auto previous_size = errors_.size();
            errors_.clear();
            return previous_size;
        }

        /** @brief Returns the current number of registered errors. */
        [[nodiscard]] std::size_t size() const
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
                    "Register capacity must be in [1, max_register_capacity]");
            }

            if (expected_size > capacity_limit)
            {
                throw std::invalid_argument(
                    "Register expected size exceeds its capacity limit");
            }
        }

        std::size_t capacity_limit_ = max_register_capacity;
        [[no_unique_address]] mutable typename Policy::Mutex mutex_;
        std::unordered_map<std::uint32_t, Error> errors_;
    };

    /** @brief Compatibility alias for the original thread-safe register name. */
    template<Category RegisterCategory>
    using MemoryRegister = Register<RegisterCategory, register_policy::ThreadSafe>;

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

    namespace system_policy
    {
        /** @brief Selects direct single-threaded routing. */
        struct SingleThreaded
        {
        };

        /** @brief Selects routing serialized independently per register. */
        struct MultiThreaded
        {
        };

        /** @brief Selects routing through a caller-owned executor. */
        template<AsyncExecutor Executor>
        struct Async
        {
            using ExecutorType = Executor;
        };
    }

    namespace detail
    {
        template<typename Policy, RegisterType... Registers>
        struct SystemSelector;

        template<RegisterType... Registers>
        struct SystemSelector<system_policy::SingleThreaded, Registers...>
        {
            using Type = SingleThreadedSystem<Registers...>;
        };

        template<RegisterType... Registers>
        struct SystemSelector<system_policy::MultiThreaded, Registers...>
        {
            using Type = MultiThreadedSystem<Registers...>;
        };

        template<AsyncExecutor Executor, RegisterType... Registers>
        struct SystemSelector<system_policy::Async<Executor>, Registers...>
        {
            using Type = AsyncSystem<Executor, Registers...>;
        };
    }

    /**
     * @brief Unified error-system API selected by one execution policy.
     * @tparam Policy One of the policies in system_policy.
     * @tparam Registers Externally owned category registers.
     */
    template<typename Policy, RegisterType... Registers>
    using System = typename detail::SystemSelector<Policy, Registers...>::Type;

}

/** @brief Enables `std::format("{}", error)` with the stable vosp representation. */
template<>
struct std::formatter<vosp::error::Error, char>
{
    constexpr auto parse(std::format_parse_context& context)
    {
        auto iterator = context.begin();
        if (iterator != context.end() && *iterator != '}')
        {
            throw std::format_error{"vosp::error::Error does not accept format specifiers"};
        }
        return iterator;
    }

    template<typename FormatContext>
    auto format(const vosp::error::Error& error, FormatContext& context) const
    {
        return std::format_to(context.out(),
                              "[{}:{}] {}",
                              vosp::error::category_name(error.category()),
                              error.code(),
                              error.message());
    }
};
