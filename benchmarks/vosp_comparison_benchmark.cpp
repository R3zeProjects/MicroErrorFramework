#include <vosp.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace
{
    using namespace vosp::error;
    constexpr std::uint32_t operation_count = 100'000;
    constexpr std::size_t worker_count = 3;

    [[nodiscard]] double operations_per_second(
        std::uint32_t operations,
        std::chrono::microseconds elapsed)
    {
        const double seconds = static_cast<double>(elapsed.count()) / 1'000'000.0;
        return seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    }
}

int main()
{
    using Clock = std::chrono::steady_clock;

    std::unordered_set<Error, ErrorHash> baseline;
    baseline.reserve(operation_count);
    const auto baseline_start = Clock::now();
    for (std::uint32_t code = 0; code < operation_count; ++code)
    {
        baseline.emplace(Category::NETWORK, code, "benchmark");
    }
    const auto baseline_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - baseline_start);

    MemoryRegister<Category::NETWORK> register_instance{operation_count};
    const auto framework_start = Clock::now();
    std::uint32_t framework_successful = 0;
    for (std::uint32_t code = 0; code < operation_count; ++code)
    {
        if (register_instance.add(Error{Category::NETWORK, code, "benchmark"}))
        {
            ++framework_successful;
        }
    }
    const auto framework_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - framework_start);

    std::array<std::unordered_set<Error, ErrorHash>, worker_count> baseline_sets;
    std::array<std::mutex, worker_count> baseline_mutexes;
    for (auto& set : baseline_sets)
    {
        set.reserve(operation_count / worker_count);
    }

    const auto baseline_multi_start = Clock::now();
    std::array<std::jthread, worker_count> baseline_workers;
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        baseline_workers[worker] = std::jthread{
            [&, worker]
            {
                const std::uint32_t operations_per_worker =
                    operation_count / static_cast<std::uint32_t>(worker_count);
                for (std::uint32_t code = 0; code < operations_per_worker; ++code)
                {
                    const std::lock_guard lock{baseline_mutexes[worker]};
                    baseline_sets[worker].emplace(Category::NETWORK, code, "benchmark");
                }
            }
        };
    }
    for (auto& worker : baseline_workers)
    {
        worker.join();
    }
    const auto baseline_multi_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - baseline_multi_start);

    std::array<MemoryRegister<Category::NETWORK>, worker_count> framework_registers;
    for (auto& framework_register : framework_registers)
    {
        framework_register.reserve(operation_count / worker_count);
    }
    const auto framework_multi_start = Clock::now();
    std::array<std::jthread, worker_count> framework_workers;
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        framework_workers[worker] = std::jthread{
            [&, worker]
            {
                const std::uint32_t operations_per_worker =
                    operation_count / static_cast<std::uint32_t>(worker_count);
                for (std::uint32_t code = 0; code < operations_per_worker; ++code)
                {
                    static_cast<void>(framework_registers[worker].add(
                        Error{Category::NETWORK, code, "benchmark"}));
                }
            }
        };
    }
    for (auto& worker : framework_workers)
    {
        worker.join();
    }
    const auto framework_multi_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - framework_multi_start);

    std::cout << "comparison workload=100000 unique inserts\n"
              << "raw_unordered_set operations_per_second="
              << operations_per_second(operation_count, baseline_elapsed) << '\n'
              << "memory_register operations=" << framework_successful
              << " operations_per_second="
              << operations_per_second(framework_successful, framework_elapsed) << '\n'
              << "raw_mutex_sets operations_per_second="
              << operations_per_second(operation_count - operation_count % worker_count,
                                       baseline_multi_elapsed) << '\n'
              << "memory_register_parallel operations_per_second="
              << operations_per_second(operation_count - operation_count % worker_count,
                                       framework_multi_elapsed) << '\n';
}
