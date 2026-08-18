#include <vosp.hpp>

#define SPDLOG_HEADER_ONLY
#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace
{
    using namespace vosp::error;
    using namespace vosp::logger;
    constexpr std::uint32_t operation_count = 1'000'000;
    constexpr std::size_t worker_count = 4;

    class FormattingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry& entry) override
        {
            static_cast<void>(fmt::format(
                "[{}] [{}] code={} message={}",
                to_string(entry.level),
                to_string(entry.error.category()),
                entry.error.code(),
                entry.error.message()));
            ++records_;
            return true;
        }

        [[nodiscard]] std::uint32_t records() const noexcept
        {
            return records_.load();
        }

    private:
        std::atomic<std::uint32_t> records_ = 0;
    };

    template<typename Function>
    [[nodiscard]] std::uint64_t measure(Function&& function)
    {
        const auto start = std::chrono::steady_clock::now();
        function();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }

    [[nodiscard]] double throughput(
        std::uint32_t records,
        std::uint64_t elapsed_us)
    {
        const double seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
        return seconds > 0.0 ? static_cast<double>(records) / seconds : 0.0;
    }

    template<typename LoggerType>
    [[nodiscard]] std::uint64_t measure_multi(LoggerType& logger)
    {
        return measure([&]
        {
            std::array<std::jthread, worker_count> workers;
            for (std::size_t worker = 0; worker < worker_count; ++worker)
            {
                workers[worker] = std::jthread{[&, worker]
                {
                    const std::uint32_t per_worker = operation_count /
                        static_cast<std::uint32_t>(worker_count);
                    for (std::uint32_t code = 0; code < per_worker; ++code)
                    {
                        static_cast<void>(logger.info(Error{
                            Category::NETWORK,
                            static_cast<std::uint32_t>(worker * per_worker) + code,
                            "connection refused"}));
                    }
                }};
            }
        });
    }

}

int main()
{
    FormattingSink micro_sink;
    Logger micro_logger{micro_sink};
    const auto micro_single_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            static_cast<void>(micro_logger.info(Error{
                Category::NETWORK, code, "connection refused"}));
        }
    });

    const auto spd_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    spdlog::logger spd_logger{"microerror-comparison", spd_sink};
    spd_logger.set_pattern("[%l] [NETWORK] code=%v message=connection refused");
    const auto spd_single_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            spd_logger.info("{}", code);
        }
    });

    FormattingSink micro_multi_sink;
    Logger micro_multi_logger{micro_multi_sink};
    const auto micro_multi_us = measure_multi(micro_multi_logger);

    FormattingSink micro_parallel_multi_sink;
    ParallelLogger micro_parallel_multi_logger{micro_parallel_multi_sink};
    const auto micro_parallel_multi_us = measure_multi(micro_parallel_multi_logger);

    FormattingSink policy_sink;
    PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch, MinimalMetadataPolicy>
        policy_logger{policy_sink};
    const auto policy_single_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            static_cast<void>(policy_logger.info(Error{
                Category::NETWORK, code, "connection refused"}));
        }
    });
    const auto policy_single_records = policy_sink.records();
    const auto policy_multi_us = measure_multi(policy_logger);
    const auto policy_multi_records = policy_sink.records() - policy_single_records;

    FormattingSink async_sink;
    PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy>
        async_logger{async_sink};
    const auto async_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            static_cast<void>(async_logger.info(Error{
                Category::NETWORK, code, "connection refused"}));
        }
        async_logger.flush();
    });

    const auto spd_multi_us = measure([&]
    {
        std::array<std::jthread, worker_count> workers;
        for (std::size_t worker = 0; worker < worker_count; ++worker)
        {
            workers[worker] = std::jthread{[&, worker]
            {
                const std::uint32_t per_worker = operation_count /
                    static_cast<std::uint32_t>(worker_count);
                for (std::uint32_t code = 0; code < per_worker; ++code)
                {
                    spd_logger.info("{}", static_cast<std::uint32_t>(worker * per_worker) + code);
                }
            }};
        }
    });

    std::cout << "comparison=MicroErrorSystem.Logger_vs_spdlog"
              << " workload=" << operation_count << " formatted records\n"
              << "micro_single records=" << micro_sink.records()
              << " elapsed_us=" << micro_single_us
              << " records_per_second=" << throughput(micro_sink.records(), micro_single_us) << '\n'
              << "spdlog_single records=" << operation_count
              << " elapsed_us=" << spd_single_us
              << " records_per_second=" << throughput(operation_count, spd_single_us) << '\n'
              << "micro_multi records=" << micro_multi_sink.records()
              << " elapsed_us=" << micro_multi_us
              << " records_per_second=" << throughput(micro_multi_sink.records(), micro_multi_us) << '\n'
              << "micro_parallel_multi records=" << micro_parallel_multi_sink.records()
              << " elapsed_us=" << micro_parallel_multi_us
              << " records_per_second=" << throughput(
                     micro_parallel_multi_sink.records(), micro_parallel_multi_us) << '\n'
              << "policy_single records=" << policy_single_records
              << " elapsed_us=" << policy_single_us
              << " records_per_second=" << throughput(policy_single_records, policy_single_us) << '\n'
              << "policy_multi records=" << policy_multi_records
              << " elapsed_us=" << policy_multi_us
              << " records_per_second=" << throughput(policy_multi_records, policy_multi_us) << '\n'
              << "policy_async records=" << async_sink.records()
              << " elapsed_us=" << async_us
              << " records_per_second=" << throughput(async_sink.records(), async_us) << '\n'
              << "spdlog_multi records=" << operation_count
              << " elapsed_us=" << spd_multi_us
              << " records_per_second=" << throughput(operation_count, spd_multi_us) << '\n';
}
