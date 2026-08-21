#include <vosp.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using namespace vosp::async;
    using namespace vosp::error;
    using namespace vosp::logger;

    bool check(bool condition, const char* message)
    {
        if (condition)
        {
            return true;
        }
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    class BlockingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            std::unique_lock lock{mutex_};
            ++writes_;
            entered_ = true;
            entered_condition_.notify_all();
            release_condition_.wait(lock, [this] { return released_; });
            return true;
        }

        void wait_until_entered()
        {
            std::unique_lock lock{mutex_};
            entered_condition_.wait(lock, [this] { return entered_; });
        }

        void release() noexcept
        {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            release_condition_.notify_all();
        }

        [[nodiscard]] std::size_t writes() const noexcept
        {
            const std::lock_guard lock{mutex_};
            return writes_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable entered_condition_;
        std::condition_variable release_condition_;
        std::size_t writes_ = 0;
        bool entered_ = false;
        bool released_ = false;
    };

    class ThrowingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            throw std::runtime_error{"injected sink failure"};
        }
    };

    bool test_worker_shutdown_unblocks_full_queue()
    {
        IndustrialWorkerPool pool{1, 1};
        std::promise<void> active_started;
        auto active = pool.submit_cancellable(
            [&active_started](std::stop_token stop_token) -> OperationResult
            {
                active_started.set_value();
                while (!stop_token.stop_requested())
                {
                    std::this_thread::yield();
                }
                return std::unexpected(task_cancelled_error());
            });
        active_started.get_future().wait();

        auto queued = pool.submit([]() -> OperationResult { return {}; });
        std::atomic<bool> producer_finished = false;
        std::atomic<bool> producer_rejected = false;
        std::jthread blocked_producer{[&]
        {
            try
            {
                pool.dispatch([]() -> OperationResult { return {}; });
            }
            catch (const std::runtime_error&)
            {
                producer_rejected.store(true, std::memory_order_release);
            }
            producer_finished.store(true, std::memory_order_release);
        }};

        std::this_thread::sleep_for(20ms);
        if (!check(!producer_finished.load(std::memory_order_acquire),
                   "producer blocks while worker queue is full"))
        {
            pool.shutdown(ShutdownMode::CANCEL_PENDING);
            return false;
        }

        pool.shutdown(ShutdownMode::CANCEL_PENDING);
        blocked_producer.join();
        const auto active_result = active.get();
        const auto queued_result = queued.get();

        bool submit_rejected = false;
        try
        {
            static_cast<void>(pool.submit([]() -> OperationResult { return {}; }));
        }
        catch (const std::runtime_error&)
        {
            submit_rejected = true;
        }
        pool.shutdown(ShutdownMode::DRAIN);

        return check(producer_finished.load(std::memory_order_acquire),
                     "shutdown releases blocked producer") &&
               check(producer_rejected.load(std::memory_order_acquire),
                     "blocked producer observes shutdown rejection") &&
               check(!active_result && active_result.error().code() == task_cancelled_code,
                     "active cooperative task observes cancellation") &&
               check(!queued_result && queued_result.error().code() == task_cancelled_code,
                     "queued future completes with cancellation") &&
               check(submit_rejected, "submit after shutdown is rejected") &&
               check(pool.pending_tasks() == 0, "cancel shutdown clears queue");
    }

    bool test_worker_exception_contract()
    {
        IndustrialWorkerPool pool{2, 8};
        auto tracked = pool.submit([]() -> OperationResult
        {
            throw std::runtime_error{"injected tracked failure"};
        });
        pool.dispatch([]() -> OperationResult
        {
            throw std::runtime_error{"injected dispatch failure"};
        });
        pool.dispatch([]() -> OperationResult
        {
            return std::unexpected(Error{Category::NONE, 77, "reported failure"});
        });

        bool exception_propagated = false;
        try
        {
            static_cast<void>(tracked.get());
        }
        catch (const std::runtime_error&)
        {
            exception_propagated = true;
        }
        pool.wait();
        pool.shutdown(ShutdownMode::DRAIN);

        return check(exception_propagated, "tracked task exception reaches future") &&
               check(pool.failed_dispatches() == 2,
                     "throwing and failed dispatches are accounted");
    }

    bool test_worker_clear_and_concurrent_shutdown()
    {
        IndustrialWorkerPool pool{1, 2};
        std::promise<void> started;
        std::promise<void> release;
        auto release_future = release.get_future().share();
        auto active = pool.submit([&]() -> OperationResult
        {
            started.set_value();
            release_future.wait();
            return {};
        });
        started.get_future().wait();
        auto first = pool.submit([]() -> OperationResult { return {}; });
        auto second = pool.submit([]() -> OperationResult { return {}; });

        const auto cancelled = pool.clear_queue();
        release.set_value();
        if (!check(active.get().has_value(), "active task survives clear_queue") ||
            !check(cancelled == 2, "clear_queue reports cancelled count") ||
            !check(!first.get() && !second.get(), "clear_queue completes queued futures"))
        {
            pool.shutdown();
            return false;
        }

        std::jthread first_shutdown{[&] { pool.shutdown(ShutdownMode::DRAIN); }};
        std::jthread second_shutdown{[&] { pool.shutdown(ShutdownMode::CANCEL_PENDING); }};
        first_shutdown.join();
        second_shutdown.join();
        pool.shutdown();
        return check(pool.is_stopping(), "concurrent and repeated shutdown is idempotent");
    }

    bool test_worker_initiated_shutdown()
    {
        IndustrialWorkerPool pool{1, 4};
        auto future = pool.submit([&pool]() -> OperationResult
        {
            pool.shutdown(ShutdownMode::DRAIN);
            return {};
        });
        const bool succeeded = future.get().has_value();
        pool.shutdown(ShutdownMode::DRAIN);
        return check(succeeded, "worker may signal shutdown without self-join");
    }

    bool test_worker_rejects_all_submission_forms_after_shutdown()
    {
        IndustrialWorkerPool pool{1, 2};
        pool.shutdown(ShutdownMode::DRAIN);

        bool cancellable_submit_rejected = false;
        try
        {
            static_cast<void>(pool.submit_cancellable(
                [](std::stop_token) -> OperationResult { return {}; }));
        }
        catch (const std::runtime_error&)
        {
            cancellable_submit_rejected = true;
        }

        bool dispatch_rejected = false;
        try
        {
            pool.dispatch([]() -> OperationResult { return {}; });
        }
        catch (const std::runtime_error&)
        {
            dispatch_rejected = true;
        }

        bool cancellable_dispatch_rejected = false;
        try
        {
            pool.dispatch_cancellable(
                [](std::stop_token) -> OperationResult { return {}; });
        }
        catch (const std::runtime_error&)
        {
            cancellable_dispatch_rejected = true;
        }

        std::vector<IndustrialWorkerPool::Task> batch;
        batch.emplace_back([]() -> OperationResult { return {}; });
        const auto accepted = pool.dispatch_bulk(batch);

        return check(cancellable_submit_rejected,
                     "cancellable submit after shutdown is rejected") &&
               check(dispatch_rejected, "dispatch after shutdown is rejected") &&
               check(cancellable_dispatch_rejected,
                     "cancellable dispatch after shutdown is rejected") &&
               check(accepted == 0 && static_cast<bool>(batch.front()),
                     "bulk dispatch preserves work rejected after shutdown");
    }

    bool test_async_logger_backpressure_recovery()
    {
        BlockingSink sink;
        PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy> logger{sink};
        if (!logger.info(Error{Category::NETWORK, 1, "block sink"}))
        {
            return check(false, "initial async record accepted");
        }
        sink.wait_until_entered();
        for (std::size_t index = 0; index < AsyncSinkDispatch::queue_capacity; ++index)
        {
            if (!logger.info(Error{Category::NETWORK,
                    static_cast<std::uint32_t>(index + 2), "fill queue"}))
            {
                sink.release();
                return check(false, "records accepted while filling async queue");
            }
        }

        std::atomic<bool> producer_finished = false;
        std::atomic<bool> producer_accepted = false;
        std::jthread producer{[&]
        {
            producer_accepted.store(
                logger.info(Error{Category::NETWORK, 5000, "after pressure"}),
                std::memory_order_release);
            producer_finished.store(true, std::memory_order_release);
        }};
        std::this_thread::sleep_for(20ms);
        if (!check(!producer_finished.load(std::memory_order_acquire),
                   "async producer blocks at queue capacity"))
        {
            sink.release();
            producer.join();
            return false;
        }

        const auto recovery_start = std::chrono::steady_clock::now();
        sink.release();
        producer.join();
        const auto recovery = std::chrono::steady_clock::now() - recovery_start;
        logger.flush();
        logger.shutdown();

        return check(producer_accepted.load(std::memory_order_acquire),
                     "producer resumes after sink recovery") &&
               check(recovery < 5s, "backpressure recovery remains bounded") &&
               check(sink.writes() == AsyncSinkDispatch::queue_capacity + 2,
                     "all accepted async records reach sink");
    }

    bool test_async_logger_failure_and_shutdown_contract()
    {
        ThrowingSink throwing_sink;
        PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy>
            failing_logger{throwing_sink};
        if (!failing_logger.error(Error{Category::FILESYSTEM, 1, "sink throws"}))
        {
            return check(false, "async logger accepts record before sink exception");
        }
        failing_logger.flush();
        failing_logger.shutdown();
        if (!check(failing_logger.failed_records() == 1,
                   "async logger contains sink exception"))
        {
            return false;
        }

        BlockingSink sink;
        PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy> logger{sink};
        static_cast<void>(logger.info(Error{Category::NETWORK, 1, "block sink"}));
        sink.wait_until_entered();
        for (std::size_t index = 0; index < AsyncSinkDispatch::queue_capacity; ++index)
        {
            static_cast<void>(logger.info(Error{Category::NETWORK,
                static_cast<std::uint32_t>(index + 2), "fill queue"}));
        }

        std::atomic<bool> accepted_after_full = true;
        std::jthread blocked_producer{[&]
        {
            accepted_after_full.store(
                logger.info(Error{Category::NETWORK, 6000, "shutdown rejection"}),
                std::memory_order_release);
        }};
        std::this_thread::sleep_for(20ms);
        std::jthread shutdown_thread{[&] { logger.shutdown(); }};
        blocked_producer.join();
        sink.release();
        shutdown_thread.join();
        logger.shutdown();

        return check(!accepted_after_full.load(std::memory_order_acquire),
                     "shutdown rejects producer blocked by full async queue") &&
               check(!logger.info(Error{Category::NETWORK, 6001, "after shutdown"}),
                     "async write after shutdown is rejected") &&
               check(sink.writes() == AsyncSinkDispatch::queue_capacity + 1,
                     "shutdown drains records accepted before shutdown");
    }

    /** @brief Verifies that owning register queries remain valid during writes. */
    bool test_concurrent_register_queries()
    {
        constexpr std::size_t producer_count = 4;
        constexpr std::size_t errors_per_producer = 250;
        Register<Category::DATABASE> registry{producer_count * errors_per_producer};
        std::atomic<bool> invalid_snapshot = false;
        std::atomic<std::size_t> active_producers = producer_count;

        std::jthread observer{[&]
        {
            while (active_producers.load(std::memory_order_acquire) != 0)
            {
                const auto snapshot = registry.snapshot();
                if (snapshot.size() > producer_count * errors_per_producer ||
                    std::ranges::any_of(snapshot, [](const Error& error)
                    {
                        return error.category() != Category::DATABASE;
                    }))
                {
                    invalid_snapshot.store(true, std::memory_order_release);
                }
            }
        }};

        std::vector<std::jthread> producers;
        producers.reserve(producer_count);
        for (std::size_t producer = 0; producer < producer_count; ++producer)
        {
            producers.emplace_back([&, producer]
            {
                for (std::size_t index = 0; index < errors_per_producer; ++index)
                {
                    const auto code = static_cast<std::uint32_t>(
                        producer * errors_per_producer + index + 1);
                    static_cast<void>(registry.add(
                        Error{Category::DATABASE, code, "concurrent query"}));
                    const auto copy = registry.find(code);
                    if (!copy || copy->code() != code)
                    {
                        invalid_snapshot.store(true, std::memory_order_release);
                    }
                }
                active_producers.fetch_sub(1, std::memory_order_release);
            });
        }
        producers.clear();
        observer.join();

        return check(!invalid_snapshot.load(std::memory_order_acquire),
                     "concurrent register snapshots remain valid") &&
               check(registry.size() == producer_count * errors_per_producer,
                     "concurrent register retains every unique code");
    }
}

int main()
{
    return test_worker_shutdown_unblocks_full_queue() &&
           test_worker_exception_contract() &&
           test_worker_clear_and_concurrent_shutdown() &&
           test_worker_initiated_shutdown() &&
           test_worker_rejects_all_submission_forms_after_shutdown() &&
           test_async_logger_backpressure_recovery() &&
           test_async_logger_failure_and_shutdown_contract() &&
           test_concurrent_register_queries() ? 0 : 1;
}
