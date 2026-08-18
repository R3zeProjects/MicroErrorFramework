#include <vosp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <array>
#include <thread>
#include <vector>
#include <future>

int main()
{
    using namespace vosp::error;

    constexpr std::uint32_t operation_count = 100'000;
    constexpr std::size_t worker_count = 3;

    MemoryRegister<Category::NETWORK> single_register{operation_count};
    SingleThreadedSystem<decltype(single_register)> single_system{single_register};
    const auto single_start = std::chrono::steady_clock::now();
    std::uint32_t single_successful = 0;

    for (std::uint32_t code = 0; code < operation_count; ++code)
    {
        const Error error{Category::NETWORK, code, "benchmark"};
        if (single_system.add(error))
        {
            ++single_successful;
        }
    }

    const auto single_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - single_start);

    MemoryRegister<Category::NETWORK> network{operation_count / worker_count};
    MemoryRegister<Category::DATABASE> database{operation_count / worker_count};
    MemoryRegister<Category::FILESYSTEM> filesystem{operation_count / worker_count};
    MultiThreadedSystem<decltype(network), decltype(database), decltype(filesystem)>
        multi_system{network, database, filesystem};
    std::array<std::uint32_t, worker_count> successful_per_worker{};
    std::array<std::jthread, worker_count> workers;
    constexpr std::array categories{
        Category::NETWORK,
        Category::DATABASE,
        Category::FILESYSTEM
    };
    const auto multi_start = std::chrono::steady_clock::now();

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers[worker] = std::jthread{
            [&multi_system, &successful_per_worker, worker, &categories]
            {
                const auto category = categories[worker];
                const std::uint32_t operations_per_worker =
                    operation_count / static_cast<std::uint32_t>(worker_count);

                for (std::uint32_t code = 0; code < operations_per_worker; ++code)
                {
                    if (multi_system.add(Error{category, code, "benchmark"}))
                    {
                        ++successful_per_worker[worker];
                    }
                }
            }
        };
    }

    for (auto& worker : workers)
    {
        worker.join();
    }

    const auto multi_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - multi_start);

    std::uint32_t multi_successful = 0;
    for (const auto successful : successful_per_worker)
    {
        multi_successful += successful;
    }

    constexpr std::uint32_t async_operation_count = 1'000;
    vosp::async::IndustrialWorkerPool executor{3};
    MemoryRegister<Category::FILESYSTEM> async_register{async_operation_count};
    AsyncSystem<decltype(executor), decltype(async_register)> async_system{
        executor,
        async_register
    };
    std::vector<std::future<OperationResult>> tasks;
    tasks.reserve(async_operation_count);
    const auto async_start = std::chrono::steady_clock::now();

    for (std::uint32_t code = 0; code < async_operation_count; ++code)
    {
        tasks.push_back(async_system.add(
            Error{Category::FILESYSTEM, code, "async benchmark"}));
    }

    std::uint32_t async_successful = 0;
    for (auto& task : tasks)
    {
        if (task.get())
        {
            ++async_successful;
        }
    }

    const auto async_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - async_start);

    constexpr std::uint32_t worker_task_count = 100'000;
    vosp::async::IndustrialWorkerPool worker_pool{4, 1024};
    std::vector<std::future<OperationResult>> worker_futures;
    worker_futures.reserve(worker_task_count);
    const auto worker_start = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < worker_task_count; ++index)
    {
        worker_futures.push_back(worker_pool.submit([]() -> OperationResult
        {
            return {};
        }));
    }

    std::uint32_t worker_successful = 0;
    for (auto& future : worker_futures)
    {
        if (future.get())
        {
            ++worker_successful;
        }
    }
    worker_pool.shutdown(vosp::async::ShutdownMode::DRAIN);
    const auto worker_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - worker_start);

    vosp::async::IndustrialWorkerPool dispatch_pool{4, 1024};
    std::atomic<std::uint32_t> dispatched = 0;
    const auto dispatch_start = std::chrono::steady_clock::now();
    for (std::uint32_t index = 0; index < worker_task_count; ++index)
    {
        dispatch_pool.dispatch([&dispatched]() -> OperationResult
        {
            dispatched.fetch_add(1, std::memory_order_relaxed);
            return {};
        });
    }
    dispatch_pool.shutdown(vosp::async::ShutdownMode::DRAIN);
    const auto dispatch_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - dispatch_start);
    const auto dispatch_successful = dispatched.load(std::memory_order_relaxed);

    vosp::async::IndustrialWorkerPool bulk_dispatch_pool{4, 1024};
    std::atomic<std::uint32_t> bulk_dispatched = 0;
    std::size_t bulk_accepted = 0;
    const auto bulk_dispatch_start = std::chrono::steady_clock::now();
    std::vector<vosp::async::IndustrialWorkerPool::Task> bulk_tasks;
    bulk_tasks.reserve(worker_task_count);
    for (std::uint32_t index = 0; index < worker_task_count; ++index)
    {
        bulk_tasks.emplace_back([&bulk_dispatched]() -> OperationResult
        {
            bulk_dispatched.fetch_add(1, std::memory_order_relaxed);
            return {};
        });
    }
    bulk_accepted = bulk_dispatch_pool.dispatch_bulk(bulk_tasks);
    bulk_dispatch_pool.shutdown(vosp::async::ShutdownMode::DRAIN);
    const auto bulk_dispatch_elapsed = std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now() - bulk_dispatch_start);
    const auto bulk_dispatch_successful = bulk_dispatched.load(std::memory_order_relaxed);

    const auto operations_per_second = [](std::uint32_t operations,
                                          std::chrono::microseconds elapsed)
    {
        const double seconds = static_cast<double>(elapsed.count()) / 1'000'000.0;
        return seconds > 0.0 ? operations / seconds : 0.0;
    };

    std::cout << "single operations=" << single_successful
              << " elapsed_us=" << single_elapsed.count()
              << " operations_per_second="
              << operations_per_second(single_successful, single_elapsed) << '\n'
              << "multi workers=" << worker_count
              << " operations=" << multi_successful
              << " elapsed_us=" << multi_elapsed.count()
              << " operations_per_second="
              << operations_per_second(multi_successful, multi_elapsed) << '\n'
              << "async operations=" << async_successful
              << " elapsed_us=" << async_elapsed.count()
              << " operations_per_second="
              << operations_per_second(async_successful, async_elapsed) << '\n'
              << "worker_pool workers=4 tasks=" << worker_successful
              << " elapsed_us=" << worker_elapsed.count()
              << " tasks_per_second="
              << operations_per_second(worker_successful, worker_elapsed) << '\n'
              << "worker_dispatch workers=4 tasks=" << dispatch_successful
              << " elapsed_us=" << dispatch_elapsed.count()
              << " tasks_per_second="
              << operations_per_second(dispatch_successful, dispatch_elapsed) << '\n'
              << "worker_bulk_dispatch workers=4 accepted=" << bulk_accepted
              << " tasks=" << bulk_dispatch_successful
              << " elapsed_us=" << bulk_dispatch_elapsed.count()
              << " tasks_per_second="
              << operations_per_second(
                     bulk_dispatch_successful,
                     bulk_dispatch_elapsed) << '\n';
}
