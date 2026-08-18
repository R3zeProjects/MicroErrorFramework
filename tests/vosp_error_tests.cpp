#include "vosp.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace
{
    using namespace vosp::error;

    template<Category RegisterCategory>
    class TestRegister final : public CategoryRegister<RegisterCategory>
    {
    public:
        [[nodiscard]] OperationResult add(const Error& error) override
        {
            if (std::ranges::contains(errors_, error))
            {
                return std::unexpected(Error{
                    RegisterCategory,
                    duplicate_error_code,
                    "Error is already registered"
                });
            }

            errors_.push_back(error);
            return {};
        }

        [[nodiscard]] OperationResult remove(const Error& error) override
        {
            const auto it = std::ranges::find(errors_, error);
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
            return std::ranges::contains(errors_, error);
        }

    private:
        std::vector<Error> errors_;
    };

    bool check(bool condition, const char* message)
    {
        if (condition)
        {
            return true;
        }

        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    bool succeeded(const OperationResult& result, const char* message)
    {
        if (result)
        {
            return true;
        }

        std::cerr << "FAILED: " << message << " ("
                  << result.error().code() << ": "
                  << result.error().message() << ")\n";
        return false;
    }

    /** @brief Verifies Error accessors, categorization, and value comparison. */
    bool test_error()
    {
        const Error error{Category::NETWORK, 42, "connection refused"};
        const Error equal_error{Category::NETWORK, 42, "connection refused"};
        const Error different_error{Category::NETWORK, 43, "connection refused"};

        return check(error.code() == 42, "Error::code") &&
               check(error.message() == "connection refused", "Error::message") &&
               check(error.category() == Category::NETWORK, "Error::category") &&
               check(error.has_category(), "categorized error") &&
               check(error == equal_error, "Error equality") &&
               check(error != different_error, "Error inequality") &&
               check(!predefined::uncategorized_error.has_category(),
                     "uncategorized predefined error");
    }

    /** @brief Verifies successful and failed std::expected-based results. */
    bool test_result()
    {
        const Result<int> success{42};
        const Result<int> failure{std::unexpect, predefined::network_error};

        return check(success.has_value() && success.value() == 42, "successful Result") &&
               check(!failure.has_value() && failure.error() == predefined::network_error,
                     "failed Result");
    }

    /** @brief Verifies category routing, rejection, and removal in Handler. */
    bool test_handler()
    {
        TestRegister<Category::NETWORK> network;
        TestRegister<Category::DATABASE> database;
        Handler handler{network, database};

        const Error network_error{Category::NETWORK, 100, "network failure"};
        const Error database_error{Category::DATABASE, 200, "database failure"};
        const Error filesystem_error{Category::FILESYSTEM, 300, "filesystem failure"};

        return succeeded(handler.add(network_error), "Handler routes add to network") &&
               check(network.contains(network_error), "network register contains error") &&
               succeeded(handler.add(database_error), "Handler routes add to database") &&
               check(database.contains(database_error), "database register contains error") &&
               check(!handler.add(database_error), "Handler rejects duplicate error") &&
               check(handler.add(database_error).error().code() == duplicate_error_code,
                     "duplicate error code") &&
               check(!handler.add(filesystem_error), "Handler rejects unknown category") &&
               check(handler.add(filesystem_error).error().code() == missing_register_code,
                     "missing register error code") &&
               succeeded(handler.remove(network_error), "Handler routes remove") &&
               check(!network.contains(network_error), "network register removes error");
    }

    /** @brief Verifies the built-in thread-safe in-memory register. */
    bool test_memory_register()
    {
        MemoryRegister<Category::NETWORK> register_instance;
        MemoryRegister<Category::NETWORK> limited_register{0, 2};
        const Error error{Category::NETWORK, 700, "memory"};
        const Error wrong_category{Category::DATABASE, 701, "wrong category"};

        const OperationResult added = register_instance.add(error);
        const OperationResult duplicate = register_instance.add(error);
        const OperationResult removed = register_instance.remove(error);
        const OperationResult missing = register_instance.remove(error);
        const OperationResult rejected_category = register_instance.add(wrong_category);
        const OperationResult limited_one = limited_register.add(
            Error{Category::NETWORK, 702, "limited one"});
        const OperationResult limited_two = limited_register.add(
            Error{Category::NETWORK, 703, "limited two"});
        const OperationResult limited_three = limited_register.add(
            Error{Category::NETWORK, 704, "limited three"});

        return succeeded(added, "MemoryRegister add") &&
               check(!duplicate && duplicate.error().code() == duplicate_error_code,
                     "MemoryRegister duplicate") &&
               succeeded(removed, "MemoryRegister remove") &&
               check(!missing && missing.error().code() == missing_error_code,
                     "MemoryRegister missing error") &&
               check(!rejected_category &&
                         rejected_category.error().code() == register_category_error_code,
                     "MemoryRegister rejects wrong category") &&
               check(limited_one.has_value() && limited_two.has_value(),
                     "MemoryRegister accepts within capacity") &&
               check(!limited_three &&
                         limited_three.error().code() == register_capacity_error_code,
                     "MemoryRegister rejects over capacity") &&
               check(limited_register.capacity_limit() == 2,
                     "MemoryRegister reports capacity");
    }

    /**
     * @brief Minimal executor test double implementing the AsyncExecutor contract.
     */
    class AsyncTestExecutor
    {
    public:
        [[nodiscard]] std::future<OperationResult> submit(std::function<OperationResult()> job)
        {
            return std::async(std::launch::async, std::move(job));
        }
    };

    /** @brief Verifies all compile-time execution policies. */
    bool test_execution_modes()
    {
        TestRegister<Category::NETWORK> single_register;
        SingleThreadedSystem<TestRegister<Category::NETWORK>> single_system{single_register};

        const Error single_error{Category::NETWORK, 400, "single-threaded"};
        if (!succeeded(single_system.add(single_error), "single-threaded system"))
        {
            return false;
        }

        TestRegister<Category::DATABASE> multi_register;
        MultiThreadedSystem<TestRegister<Category::DATABASE>> multi_system{multi_register};

        std::vector<std::thread> workers;
        for (std::uint32_t code = 0; code < 8; ++code)
        {
            workers.emplace_back([&multi_system, code]
            {
                const Error error{Category::DATABASE, 500 + code, "multi-threaded"};
                (void)multi_system.add(error);
            });
        }

        for (auto& worker : workers)
        {
            worker.join();
        }

        if (!check(multi_register.contains(
                       Error{Category::DATABASE, 503, "multi-threaded"}),
                   "multi-threaded system"))
        {
            return false;
        }

        AsyncTestExecutor executor;
        TestRegister<Category::FILESYSTEM> async_register;
        AsyncSystem<AsyncTestExecutor, TestRegister<Category::FILESYSTEM>> async_system{
            executor,
            async_register
        };

        const Error async_error{Category::FILESYSTEM, 600, "asynchronous"};
        std::future<OperationResult> result = async_system.add(async_error);

        return succeeded(result.get(), "asynchronous system") &&
               check(async_register.contains(async_error), "asynchronous register");
    }

    /** @brief Verifies the bounded IndustrialWorkerPool executor. */
    bool test_worker_pool()
    {
        using vosp::async::IndustrialWorkerPool;
        using vosp::async::ShutdownMode;

        bool limits_checked = false;
        try
        {
            IndustrialWorkerPool invalid_workers{vosp::async::max_worker_count + 1};
        }
        catch (const std::invalid_argument&)
        {
            limits_checked = true;
        }

        if (!check(limits_checked, "worker count limit") )
        {
            return false;
        }

        IndustrialWorkerPool pool{2};
        MemoryRegister<Category::FILESYSTEM> register_instance{8};
        AsyncSystem<decltype(pool), decltype(register_instance)> system{
            pool,
            register_instance
        };
        std::vector<std::future<OperationResult>> tasks;

        for (std::uint32_t code = 0; code < 8; ++code)
        {
            tasks.push_back(system.add(
                Error{Category::FILESYSTEM, 800 + code, "worker pool"}));
        }

        bool all_succeeded = true;
        for (auto& task : tasks)
        {
            all_succeeded = static_cast<bool>(task.get()) && all_succeeded;
        }

        if (!check(pool.worker_count() == 2, "worker count") ||
            !check(pool.queue_capacity() == vosp::async::max_queue_capacity,
                   "queue capacity limit") ||
            !check(all_succeeded, "worker pool task results") ||
            !check(register_instance.size() == 8, "worker pool registration"))
        {
            return false;
        }

        IndustrialWorkerPool cancellable_pool{1, 2};
        std::promise<void> release;
        std::shared_future<void> release_signal = release.get_future().share();
        std::future<OperationResult> running = cancellable_pool.submit(
            [release_signal]() -> OperationResult
            {
                release_signal.wait();
                return {};
            });
        std::future<OperationResult> queued_one = cancellable_pool.submit(
            []() -> OperationResult { return {}; });
        std::future<OperationResult> queued_two = cancellable_pool.submit(
            []() -> OperationResult { return {}; });

        const std::size_t cancelled = cancellable_pool.clear_queue();
        release.set_value();

        const OperationResult running_result = running.get();
        const OperationResult cancelled_one = queued_one.get();
        const OperationResult cancelled_two = queued_two.get();
        cancellable_pool.shutdown(ShutdownMode::CANCEL_PENDING);

        if (!check(cancelled == 2, "clear_queue cancels pending tasks") ||
            !check(running_result.has_value(), "running task completes") ||
            !check(!cancelled_one &&
                       cancelled_one.error().code() == vosp::async::task_cancelled_code,
                   "first queued task cancellation") ||
            !check(!cancelled_two &&
                       cancelled_two.error().code() == vosp::async::task_cancelled_code,
                   "second queued task cancellation"))
        {
            return false;
        }

        IndustrialWorkerPool cooperative_pool{1};
        auto started = std::make_shared<std::promise<void>>();
        std::future<OperationResult> cooperative = cooperative_pool.submit_cancellable(
            [started](std::stop_token stop_token) -> OperationResult
            {
                started->set_value();
                while (!stop_token.stop_requested())
                {
                    std::this_thread::yield();
                }

                return std::unexpected(vosp::async::task_cancelled_error());
            });
        started->get_future().wait();
        cooperative_pool.shutdown(ShutdownMode::CANCEL_PENDING);

        const OperationResult cooperative_result = cooperative.get();
        if (!check(!cooperative_result &&
                         cooperative_result.error().code() == vosp::async::task_cancelled_code,
                     "cooperative cancellation") &&
               check(cooperative_result.error().category() == Category::NONE,
                     "cooperative cancellation category"))
        {
            return false;
        }

        bool capacity_rejected = false;
        try
        {
            MemoryRegister<Category::NETWORK> oversized{
                max_register_capacity + 1
            };
        }
        catch (const std::invalid_argument&)
        {
            capacity_rejected = true;
        }

        if (!check(capacity_rejected, "register capacity limit") )
        {
            return false;
        }

        IndustrialWorkerPool lifetime_pool{1, 2};
        MemoryRegister<Category::NETWORK> lifetime_register;
        std::promise<void> release_lifetime_task;
        std::shared_future<void> lifetime_signal = release_lifetime_task.get_future().share();
        std::future<OperationResult> blocker = lifetime_pool.submit(
            [lifetime_signal]() -> OperationResult
            {
                lifetime_signal.wait();
                return {};
            });
        std::future<OperationResult> after_system_destruction;
        const Error lifetime_error{Category::NETWORK, 9001, "system lifetime"};
        {
            AsyncSystem<decltype(lifetime_pool), decltype(lifetime_register)> lifetime_system{
                lifetime_pool,
                lifetime_register
            };
            after_system_destruction = lifetime_system.add(lifetime_error);
        }
        release_lifetime_task.set_value();

        if (!check(blocker.get().has_value(), "lifetime blocker completes") ||
            !check(after_system_destruction.get().has_value(),
                   "async task survives system destruction") ||
            !check(lifetime_register.contains(lifetime_error),
                   "async task updates surviving register"))
        {
            return false;
        }

        IndustrialWorkerPool self_shutdown_pool{2};
        std::future<OperationResult> self_shutdown = self_shutdown_pool.submit(
            [&self_shutdown_pool]() -> OperationResult
            {
                self_shutdown_pool.shutdown();
                return {};
            });

        return check(self_shutdown.get().has_value(), "worker initiated shutdown") &&
               check(self_shutdown_pool.is_stopping(), "worker pool reports stopping") &&
               check(self_shutdown_pool.active_tasks() == 0,
                     "worker pool reports no active tasks") &&
               check(self_shutdown_pool.pending_tasks() == 0,
                     "worker shutdown clears pending queue");
    }
}

int main()
{
    return test_error() && test_result() && test_handler() &&
           test_memory_register() && test_execution_modes() &&
           test_worker_pool() ? 0 : 1;
}
