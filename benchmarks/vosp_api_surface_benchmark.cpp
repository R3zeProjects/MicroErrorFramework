#include <vosp.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <streambuf>
#include <string_view>

namespace
{
    using namespace vosp::error;
    using namespace vosp::logger;

    constexpr std::size_t logger_operations = 2'000'000;
    constexpr std::size_t sink_operations = 500'000;

    using LegacyRegister = MemoryRegister<Category::NETWORK>;
    using UnifiedRegister = Register<Category::NETWORK>;
    using LegacySystem = MultiThreadedSystem<LegacyRegister>;
    using UnifiedSystem =
        System<system_policy::MultiThreaded, UnifiedRegister>;
    using LegacyLogger =
        PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch, MinimalMetadataPolicy>;
    using UnifiedLogger =
        Logger<logger_policy::AcceptAll,
               logger_policy::Parallel,
               logger_policy::MinimalMetadata>;

    static_assert(std::same_as<LegacyRegister, UnifiedRegister>);
    static_assert(std::same_as<LegacySystem, UnifiedSystem>);
    static_assert(std::same_as<vosp::async::IndustrialWorkerPool,
                               vosp::async::WorkerPool>);
    static_assert(std::same_as<LegacyLogger, UnifiedLogger>);
    static_assert(std::same_as<ConsoleSink, Sink<sink_policy::Immediate>>);

    class CountingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            ++records_;
            return true;
        }

        [[nodiscard]] std::size_t records() const noexcept { return records_; }

    private:
        std::size_t records_ = 0;
    };

    class NullBuffer final : public std::streambuf
    {
    protected:
        std::streamsize xsputn(const char*, std::streamsize count) override
        {
            return count;
        }

        int_type overflow(int_type character) override { return character; }
    };

    template<typename Operation>
    [[nodiscard]] double measure(std::size_t operations, Operation&& operation)
    {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < operations; ++index)
        {
            operation();
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        return static_cast<double>(operations) / elapsed;
    }

    template<typename LoggerType>
    void benchmark_logger(
        std::string_view name,
        LoggerType& logger,
        CountingSink& sink)
    {
        const Error error{Category::NETWORK, 1001, "api-surface"};
        const auto before = sink.records();
        const auto throughput = measure(logger_operations, [&]
        {
            static_cast<void>(logger.info(error));
        });
        const auto delivered = sink.records() - before;
        std::cout << "api=" << name
                  << " operations=" << delivered
                  << " operations_per_second=" << throughput
                  << " object_bytes=" << sizeof(LoggerType) << '\n';
    }

    template<typename SinkType>
    void benchmark_sink(std::string_view name, SinkType& sink)
    {
        const LogEntry entry{
            {},
            {},
            Level::INFO,
            Error{Category::NETWORK, 1002, "api-surface"}
        };
        std::size_t accepted = 0;
        const auto throughput = measure(sink_operations, [&]
        {
            accepted += static_cast<std::size_t>(sink.write(entry));
        });
        std::cout << "api=" << name
                  << " operations=" << accepted
                  << " operations_per_second=" << throughput
                  << " object_bytes=" << sizeof(SinkType) << '\n';
    }
}

int main(int argc, char** argv)
{
    const bool reverse = argc > 1 && std::string_view{argv[1]} == "--reverse";

    CountingSink legacy_counter;
    CountingSink unified_counter;
    LegacyLogger legacy_logger{legacy_counter};
    UnifiedLogger unified_logger{unified_counter};

    NullBuffer legacy_buffer;
    NullBuffer unified_buffer;
    std::ostream legacy_stream{&legacy_buffer};
    std::ostream unified_stream{&unified_buffer};
    ConsoleSink legacy_sink{legacy_stream};
    Sink unified_sink{unified_stream};

    if (reverse)
    {
        benchmark_logger("unified_logger", unified_logger, unified_counter);
        benchmark_logger("legacy_logger", legacy_logger, legacy_counter);
        benchmark_sink("unified_sink", unified_sink);
        benchmark_sink("legacy_sink", legacy_sink);
    }
    else
    {
        benchmark_logger("legacy_logger", legacy_logger, legacy_counter);
        benchmark_logger("unified_logger", unified_logger, unified_counter);
        benchmark_sink("legacy_sink", legacy_sink);
        benchmark_sink("unified_sink", unified_sink);
    }
}
