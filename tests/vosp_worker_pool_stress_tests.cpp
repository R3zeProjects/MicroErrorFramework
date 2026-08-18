#include "vosp.hpp"

#include <atomic>
#include <cstddef>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    using vosp::error::OperationResult;

    bool check(bool condition, const char* message)
    {
        if (condition)
        {
            return true;
        }

        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
}

int main()
{
    constexpr std::size_t producer_count = 8;
    constexpr std::size_t tasks_per_producer = 250;
    constexpr std::size_t expected_tasks = producer_count * tasks_per_producer;

    vosp::async::IndustrialWorkerPool pool{4, 32};
    std::atomic<std::size_t> executed = 0;
    std::mutex futures_mutex;
    std::vector<std::future<OperationResult>> futures;
    futures.reserve(expected_tasks);
    std::vector<std::jthread> producers;
    producers.reserve(producer_count);

    for (std::size_t producer = 0; producer < producer_count; ++producer)
    {
        producers.emplace_back([&]
        {
            for (std::size_t task = 0; task < tasks_per_producer; ++task)
            {
                auto future = pool.submit([&]() -> OperationResult
                {
                    executed.fetch_add(1, std::memory_order_relaxed);
                    return {};
                });

                const std::lock_guard lock{futures_mutex};
                futures.push_back(std::move(future));
            }
        });
    }

    for (auto& producer : producers)
    {
        producer.join();
    }

    bool all_succeeded = futures.size() == expected_tasks;
    for (auto& future : futures)
    {
        all_succeeded = future.get().has_value() && all_succeeded;
    }

    pool.shutdown(vosp::async::ShutdownMode::DRAIN);

    const bool passed =
        check(futures.size() == expected_tasks, "all tasks submitted") &&
        check(all_succeeded, "all stress tasks succeeded") &&
        check(executed.load(std::memory_order_relaxed) == expected_tasks,
              "all stress tasks executed") &&
        check(pool.pending_tasks() == 0, "queue drained");
    return passed ? 0 : 1;
}
