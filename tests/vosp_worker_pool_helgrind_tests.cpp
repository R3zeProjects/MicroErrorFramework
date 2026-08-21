#include <vosp.hpp>

#include <cstddef>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using vosp::error::OperationResult;

    class GuardedCounter final
    {
    public:
        void increment()
        {
            const std::lock_guard lock{mutex_};
            ++value_;
        }

        [[nodiscard]] std::size_t value() const
        {
            const std::lock_guard lock{mutex_};
            return value_;
        }

    private:
        mutable std::mutex mutex_;
        std::size_t value_ = 0;
    };
}

int main()
{
    constexpr std::size_t producer_count = 6;
    constexpr std::size_t tasks_per_producer = 300;
    constexpr std::size_t bulk_task_count = 600;
    constexpr std::size_t expected =
        producer_count * tasks_per_producer + bulk_task_count;

    vosp::async::WorkerPool pool{4, 32};
    GuardedCounter completed;
    std::vector<std::jthread> producers;
    producers.reserve(producer_count);

    for (std::size_t producer = 0; producer < producer_count; ++producer)
    {
        producers.emplace_back([&pool, &completed]
        {
            for (std::size_t task = 0; task < tasks_per_producer; ++task)
            {
                pool.dispatch([&completed]() -> OperationResult
                {
                    completed.increment();
                    return {};
                });
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    std::vector<vosp::async::WorkerPool::Task> bulk_tasks;
    bulk_tasks.reserve(bulk_task_count);
    for (std::size_t task = 0; task < bulk_task_count; ++task)
    {
        bulk_tasks.emplace_back([&completed]() -> OperationResult
        {
            completed.increment();
            return {};
        });
    }

    if (pool.dispatch_bulk(bulk_tasks) != bulk_task_count)
    {
        std::cerr << "FAILED: bulk dispatch stopped before accepting every task\n";
        return 1;
    }

    pool.wait();
    pool.shutdown(vosp::async::ShutdownMode::DRAIN);

    if (completed.value() != expected)
    {
        std::cerr << "FAILED: completed " << completed.value()
                  << " of " << expected << " tasks\n";
        return 1;
    }

    if (pool.pending_tasks() != 0 || pool.active_tasks() != 0)
    {
        std::cerr << "FAILED: shutdown left tracked work behind\n";
        return 1;
    }

    std::cout << "WorkerPool Helgrind workload passed: " << expected
              << " tasks\n";
    return 0;
}
