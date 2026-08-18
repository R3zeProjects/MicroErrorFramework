#include <vosp.hpp>

#include <BS_thread_pool.hpp>
#include <taskflow/taskflow.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
    using vosp::async::IndustrialWorkerPool;
    using vosp::async::ShutdownMode;
    using vosp::error::OperationResult;

    constexpr std::uint32_t operation_count = 100'000;
    constexpr std::size_t worker_count = 4;

    template<typename Function>
    [[nodiscard]] std::uint64_t measure(Function&& function)
    {
        const auto start = std::chrono::steady_clock::now();
        std::forward<Function>(function)();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }

    [[nodiscard]] double throughput(std::uint64_t tasks, std::uint64_t elapsed_us)
    {
        const auto seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
        return seconds > 0.0 ? static_cast<double>(tasks) / seconds : 0.0;
    }

    void print_result(std::string_view name, std::uint64_t elapsed_us)
    {
        std::cout << name
                  << " tasks=" << operation_count
                  << " elapsed_us=" << elapsed_us
                  << " tasks_per_second=" << throughput(operation_count, elapsed_us)
                  << '\n';
    }

    void warm_up(IndustrialWorkerPool& pool)
    {
        std::vector<std::future<OperationResult>> futures;
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            futures.push_back(pool.submit([]() -> OperationResult { return {}; }));
        }
        for (auto& future : futures)
        {
            static_cast<void>(future.get());
        }
    }

    void warm_up(BS::thread_pool<>& pool)
    {
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            pool.detach_task([] {});
        }
        pool.wait();
    }

    void warm_up(tf::Executor& executor)
    {
        for (std::size_t index = 0; index < worker_count; ++index)
        {
            executor.silent_async([] {});
        }
        executor.wait_for_all();
    }
}

int main()
{
    IndustrialWorkerPool vosp_tracked_pool{worker_count, 1024};
    warm_up(vosp_tracked_pool);
    std::atomic<std::uint32_t> vosp_tracked_count = 0;
    const auto vosp_tracked_us = measure([&]
    {
        std::vector<std::future<OperationResult>> futures;
        futures.reserve(operation_count);
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            futures.push_back(vosp_tracked_pool.submit([&]() -> OperationResult
            {
                vosp_tracked_count.fetch_add(1, std::memory_order_relaxed);
                return {};
            }));
        }
        for (auto& future : futures)
        {
            static_cast<void>(future.get());
        }
    });

    BS::thread_pool<> bs_tracked_pool{worker_count};
    warm_up(bs_tracked_pool);
    std::atomic<std::uint32_t> bs_tracked_count = 0;
    const auto bs_tracked_us = measure([&]
    {
        std::vector<std::future<void>> futures;
        futures.reserve(operation_count);
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            futures.push_back(bs_tracked_pool.submit_task([&]
            {
                bs_tracked_count.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (auto& future : futures)
        {
            future.get();
        }
    });

    tf::Executor taskflow_tracked_executor{worker_count};
    warm_up(taskflow_tracked_executor);
    std::atomic<std::uint32_t> taskflow_tracked_count = 0;
    const auto taskflow_tracked_us = measure([&]
    {
        std::vector<std::future<void>> futures;
        futures.reserve(operation_count);
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            futures.push_back(taskflow_tracked_executor.async([&]
            {
                taskflow_tracked_count.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        for (auto& future : futures)
        {
            future.get();
        }
    });

    IndustrialWorkerPool vosp_dispatch_pool{worker_count, 1024};
    warm_up(vosp_dispatch_pool);
    std::atomic<std::uint32_t> vosp_dispatch_count = 0;
    const auto vosp_dispatch_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            vosp_dispatch_pool.dispatch([&]() -> OperationResult
            {
                vosp_dispatch_count.fetch_add(1, std::memory_order_relaxed);
                return {};
            });
        }
        vosp_dispatch_pool.wait();
    });

    BS::thread_pool<> bs_dispatch_pool{worker_count};
    warm_up(bs_dispatch_pool);
    std::atomic<std::uint32_t> bs_dispatch_count = 0;
    const auto bs_dispatch_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            bs_dispatch_pool.detach_task([&]
            {
                bs_dispatch_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        bs_dispatch_pool.wait();
    });

    tf::Executor taskflow_dispatch_executor{worker_count};
    warm_up(taskflow_dispatch_executor);
    std::atomic<std::uint32_t> taskflow_dispatch_count = 0;
    const auto taskflow_dispatch_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            taskflow_dispatch_executor.silent_async([&]
            {
                taskflow_dispatch_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        taskflow_dispatch_executor.wait_for_all();
    });

    IndustrialWorkerPool vosp_bulk_pool{worker_count, 1024};
    warm_up(vosp_bulk_pool);
    std::atomic<std::uint32_t> vosp_bulk_count = 0;
    std::size_t vosp_bulk_accepted = 0;
    const auto vosp_bulk_us = measure([&]
    {
        std::vector<IndustrialWorkerPool::Task> tasks;
        tasks.reserve(operation_count);
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            tasks.emplace_back([&]() -> OperationResult
            {
                vosp_bulk_count.fetch_add(1, std::memory_order_relaxed);
                return {};
            });
        }
        vosp_bulk_accepted = vosp_bulk_pool.dispatch_bulk(tasks);
        vosp_bulk_pool.wait();
    });

    BS::thread_pool<> bs_bulk_pool{worker_count};
    warm_up(bs_bulk_pool);
    std::atomic<std::uint32_t> bs_bulk_count = 0;
    const auto bs_bulk_us = measure([&]
    {
        std::vector<std::function<void()>> tasks;
        tasks.reserve(operation_count);
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            tasks.emplace_back([&]
            {
                bs_bulk_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        bs_bulk_pool.detach_bulk(tasks);
        bs_bulk_pool.wait();
    });

    const bool valid =
        vosp_tracked_count.load(std::memory_order_relaxed) == operation_count &&
        bs_tracked_count.load(std::memory_order_relaxed) == operation_count &&
        taskflow_tracked_count.load(std::memory_order_relaxed) == operation_count &&
        vosp_dispatch_count.load(std::memory_order_relaxed) == operation_count &&
        bs_dispatch_count.load(std::memory_order_relaxed) == operation_count &&
        taskflow_dispatch_count.load(std::memory_order_relaxed) == operation_count &&
        vosp_bulk_accepted == operation_count &&
        vosp_bulk_count.load(std::memory_order_relaxed) == operation_count &&
        bs_bulk_count.load(std::memory_order_relaxed) == operation_count;
    if (!valid)
    {
        std::cerr << "worker pool comparison validation failed\n";
        return 1;
    }

    std::cout << "comparison=worker_pools operations=" << operation_count
              << " workers=" << worker_count << '\n';
    print_result("tracked_vosp", vosp_tracked_us);
    print_result("tracked_bs_thread_pool", bs_tracked_us);
    print_result("tracked_taskflow", taskflow_tracked_us);
    print_result("dispatch_vosp", vosp_dispatch_us);
    print_result("dispatch_bs_thread_pool", bs_dispatch_us);
    print_result("dispatch_taskflow", taskflow_dispatch_us);
    print_result("bulk_vosp", vosp_bulk_us);
    print_result("bulk_bs_thread_pool", bs_bulk_us);

    vosp_tracked_pool.shutdown(ShutdownMode::DRAIN);
    vosp_dispatch_pool.shutdown(ShutdownMode::DRAIN);
    vosp_bulk_pool.shutdown(ShutdownMode::DRAIN);
    return 0;
}
