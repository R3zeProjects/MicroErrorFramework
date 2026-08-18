#include <vosp.hpp>

#define SPDLOG_HEADER_ONLY
#include <spdlog/details/null_mutex.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <string_view>
#include <thread>

namespace
{
    using namespace vosp::error;
    using namespace vosp::logger;

    constexpr std::uint32_t operation_count = 3'000'000;
    constexpr std::size_t worker_count = 4;

    class alignas(64) CountingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) noexcept override
        {
            records_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] std::uint64_t records() const noexcept
        {
            return records_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<std::uint64_t> records_ = 0;
    };

    class ShardedCountingSink final : public ILogSink
    {
    private:
        static constexpr std::size_t maximum_slots = 64;

        struct alignas(64) Counter
        {
            std::uint64_t value = 0;
        };

        struct LocalSlot
        {
            std::uint64_t generation = 0;
            std::size_t index = 0;
        };

    public:
        ShardedCountingSink() noexcept
            : generation_{generation_source_.fetch_add(1, std::memory_order_relaxed)}
        {
        }

        [[nodiscard]] bool write(const LogEntry&) noexcept override
        {
            thread_local LocalSlot local_slot;
            if (local_slot.generation != generation_)
            {
                local_slot.generation = generation_;
                local_slot.index = next_slot_.fetch_add(1, std::memory_order_relaxed);
            }

            if (local_slot.index < counters_.size())
            {
                ++counters_[local_slot.index].value;
            }
            else
            {
                overflow_.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }

        [[nodiscard]] std::uint64_t records() const noexcept
        {
            std::uint64_t total = overflow_.load(std::memory_order_relaxed);
            for (const auto& counter : counters_)
            {
                total += counter.value;
            }
            return total;
        }

    private:
        inline static std::atomic<std::uint64_t> generation_source_ = 1;
        const std::uint64_t generation_;
        std::atomic<std::size_t> next_slot_ = 0;
        std::array<Counter, maximum_slots> counters_{};
        std::atomic<std::uint64_t> overflow_ = 0;
    };

    class FormattingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry& entry) override
        {
            fmt::memory_buffer buffer;
            fmt::format_to(
                std::back_inserter(buffer),
                "[{}] [{}] code={} message={}",
                to_string(entry.level),
                to_string(entry.error.category()),
                entry.error.code(),
                entry.error.message());
            bytes_.fetch_add(buffer.size(), std::memory_order_relaxed);
            records_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] std::uint64_t records() const noexcept
        {
            return records_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t bytes() const noexcept
        {
            return bytes_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<std::uint64_t> records_ = 0;
        std::atomic<std::uint64_t> bytes_ = 0;
    };

    class SpdCountingSink final
        : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
    {
    public:
        [[nodiscard]] std::uint64_t records() const noexcept
        {
            return records_.load(std::memory_order_relaxed);
        }

    protected:
        void sink_it_(const spdlog::details::log_msg&) override
        {
            records_.fetch_add(1, std::memory_order_relaxed);
        }

        void flush_() override {}

    private:
        std::atomic<std::uint64_t> records_ = 0;
    };

    class SpdFormattingSink final
        : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
    {
    public:
        [[nodiscard]] std::uint64_t records() const noexcept
        {
            return records_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::uint64_t bytes() const noexcept
        {
            return bytes_.load(std::memory_order_relaxed);
        }

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override
        {
            fmt::memory_buffer buffer;
            fmt::format_to(
                std::back_inserter(buffer),
                "[{}] [NETWORK] code={} message=connection refused",
                spdlog::level::to_string_view(message.level),
                message.payload);
            bytes_.fetch_add(buffer.size(), std::memory_order_relaxed);
            records_.fetch_add(1, std::memory_order_relaxed);
        }

        void flush_() override {}

    private:
        std::atomic<std::uint64_t> records_ = 0;
        std::atomic<std::uint64_t> bytes_ = 0;
    };

    class NullStreamBuffer final : public std::streambuf
    {
    protected:
        std::streamsize xsputn(const char*, std::streamsize count) override
        {
            return count;
        }

        int_type overflow(int_type value) override
        {
            return traits_type::not_eof(value);
        }
    };

    class LegacyConsoleSink final : public ILogSink
    {
    public:
        explicit LegacyConsoleSink(std::ostream& output) noexcept
            : output_(output)
        {
        }

        [[nodiscard]] bool write(const LogEntry& entry) override
        {
            const std::lock_guard lock{mutex_};
            output_ << '[' << to_string(entry.level) << "] ["
                    << to_string(entry.error.category()) << "] code="
                    << entry.error.code() << " message="
                    << entry.error.message() << '\n';
            return static_cast<bool>(output_);
        }

    private:
        std::ostream& output_;
        std::mutex mutex_;
    };

    template<typename Function>
    [[nodiscard]] std::uint64_t measure(Function&& function)
    {
        const auto start = std::chrono::steady_clock::now();
        std::forward<Function>(function)();
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    }

    template<typename Function>
    [[nodiscard]] std::uint64_t measure_multi(Function&& operation)
    {
        std::latch ready{worker_count};
        std::latch start{1};
        std::array<std::jthread, worker_count> workers;
        for (std::size_t worker = 0; worker < worker_count; ++worker)
        {
            workers[worker] = std::jthread{[&operation, &ready, &start, worker]
            {
                ready.count_down();
                start.wait();
                const auto records_per_worker = operation_count /
                    static_cast<std::uint32_t>(worker_count);
                for (std::uint32_t code = 0; code < records_per_worker; ++code)
                {
                    operation(
                        worker,
                        static_cast<std::uint32_t>(worker * records_per_worker) + code);
                }
            }};
        }

        ready.wait();
        const auto start_time = std::chrono::steady_clock::now();
        start.count_down();
        for (auto& worker : workers)
        {
            worker.join();
        }
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time).count());
    }

    [[nodiscard]] double throughput(std::uint64_t records, std::uint64_t elapsed_us)
    {
        const double seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
        return seconds > 0.0 ? static_cast<double>(records) / seconds : 0.0;
    }

    void print_result(
        std::string_view name,
        std::uint64_t records,
        std::uint64_t elapsed_us,
        std::uint64_t bytes = 0)
    {
        std::cout << name
                  << " records=" << records
                  << " elapsed_us=" << elapsed_us
                  << " records_per_second=" << throughput(records, elapsed_us);
        if (bytes != 0)
        {
            std::cout << " formatted_bytes=" << bytes;
        }
        std::cout << '\n';
    }
}

int main()
{
    const Error prepared_error{Category::NETWORK, 42, "prepared message"};

    CountingSink micro_dispatch_single_sink;
    ParallelLogger micro_dispatch_single{micro_dispatch_single_sink};
    const auto micro_dispatch_single_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            static_cast<void>(micro_dispatch_single.info(prepared_error));
        }
    });

    auto spd_dispatch_single_sink = std::make_shared<SpdCountingSink>();
    spdlog::logger spd_dispatch_single{"spd-dispatch-single", spd_dispatch_single_sink};
    const auto spd_dispatch_single_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            spd_dispatch_single.info("prepared message");
        }
    });

    CountingSink micro_dispatch_multi_sink;
    ParallelLogger micro_dispatch_multi{micro_dispatch_multi_sink};
    const auto micro_dispatch_multi_us = measure_multi([&](std::size_t, std::uint32_t)
    {
        static_cast<void>(micro_dispatch_multi.info(prepared_error));
    });

    auto spd_dispatch_multi_sink = std::make_shared<SpdCountingSink>();
    spdlog::logger spd_dispatch_multi{"spd-dispatch-multi", spd_dispatch_multi_sink};
    const auto spd_dispatch_multi_us = measure_multi([&](std::size_t, std::uint32_t)
    {
        spd_dispatch_multi.info("prepared message");
    });

    ShardedCountingSink micro_sharded_sink;
    ParallelLogger micro_sharded_logger{micro_sharded_sink};
    const auto micro_sharded_us = measure_multi([&](std::size_t, std::uint32_t)
    {
        static_cast<void>(micro_sharded_logger.info(prepared_error));
    });

    std::array<CountingSink, worker_count> partitioned_sinks;
    std::array<std::unique_ptr<ParallelLogger>, worker_count> partitioned_loggers;
    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        partitioned_loggers[worker] = std::make_unique<ParallelLogger>(
            partitioned_sinks[worker]);
    }
    const auto micro_partitioned_us = measure_multi(
        [&](std::size_t worker, std::uint32_t)
        {
            static_cast<void>(partitioned_loggers[worker]->info(prepared_error));
        });
    std::uint64_t partitioned_records = 0;
    for (const auto& sink : partitioned_sinks)
    {
        partitioned_records += sink.records();
    }

    FormattingSink micro_format_single_sink;
    ParallelLogger micro_format_single{micro_format_single_sink};
    const auto micro_format_single_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            static_cast<void>(micro_format_single.info(
                Error{Category::NETWORK, code, "connection refused"}));
        }
    });

    auto spd_format_single_sink = std::make_shared<SpdFormattingSink>();
    spdlog::logger spd_format_single{"spd-format-single", spd_format_single_sink};
    const auto spd_format_single_us = measure([&]
    {
        for (std::uint32_t code = 0; code < operation_count; ++code)
        {
            spd_format_single.info("{}", code);
        }
    });

    FormattingSink micro_format_multi_sink;
    ParallelLogger micro_format_multi{micro_format_multi_sink};
    const auto micro_format_multi_us = measure_multi([&](std::size_t, std::uint32_t code)
    {
        static_cast<void>(micro_format_multi.info(
            Error{Category::NETWORK, code, "connection refused"}));
    });

    auto spd_format_multi_sink = std::make_shared<SpdFormattingSink>();
    spdlog::logger spd_format_multi{"spd-format-multi", spd_format_multi_sink};
    const auto spd_format_multi_us = measure_multi([&](std::size_t, std::uint32_t code)
    {
        spd_format_multi.info("{}", code);
    });

    NullStreamBuffer optimized_buffer;
    std::ostream optimized_output{&optimized_buffer};
    ConsoleSink optimized_console{optimized_output};
    const LogEntry console_entry{
        {}, {}, Level::INFO, Error{Category::NETWORK, 42, "connection refused"}};
    const auto optimized_console_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            static_cast<void>(optimized_console.write(console_entry));
        }
    });

    NullStreamBuffer legacy_buffer;
    std::ostream legacy_output{&legacy_buffer};
    LegacyConsoleSink legacy_console{legacy_output};
    const auto legacy_console_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            static_cast<void>(legacy_console.write(console_entry));
        }
    });

    NullStreamBuffer optimized_multi_buffer;
    std::ostream optimized_multi_output{&optimized_multi_buffer};
    ConsoleSink optimized_multi_console{optimized_multi_output};
    const auto optimized_multi_console_us = measure_multi(
        [&](std::size_t, std::uint32_t)
        {
            static_cast<void>(optimized_multi_console.write(console_entry));
        });

    NullStreamBuffer buffered_stream_buffer;
    std::ostream buffered_stream_output{&buffered_stream_buffer};
    BufferedStreamSink buffered_stream_sink{buffered_stream_output};
    const auto buffered_stream_single_us = measure([&]
    {
        for (std::uint32_t index = 0; index < operation_count; ++index)
        {
            static_cast<void>(buffered_stream_sink.write(console_entry));
        }
        static_cast<void>(buffered_stream_sink.flush());
    });

    NullStreamBuffer buffered_multi_buffer;
    std::ostream buffered_multi_output{&buffered_multi_buffer};
    BufferedStreamSink buffered_multi_sink{buffered_multi_output};
    auto buffered_multi_us = measure_multi(
        [&](std::size_t, std::uint32_t)
        {
            static_cast<void>(buffered_multi_sink.write(console_entry));
        });
    buffered_multi_us += measure([&]
    {
        static_cast<void>(buffered_multi_sink.flush());
    });

    NullStreamBuffer legacy_multi_buffer;
    std::ostream legacy_multi_output{&legacy_multi_buffer};
    LegacyConsoleSink legacy_multi_console{legacy_multi_output};
    const auto legacy_multi_console_us = measure_multi(
        [&](std::size_t, std::uint32_t)
        {
            static_cast<void>(legacy_multi_console.write(console_entry));
        });

    const bool valid =
        micro_dispatch_single_sink.records() == operation_count &&
        spd_dispatch_single_sink->records() == operation_count &&
        micro_dispatch_multi_sink.records() == operation_count &&
        spd_dispatch_multi_sink->records() == operation_count &&
        micro_sharded_sink.records() == operation_count &&
        partitioned_records == operation_count &&
        micro_format_single_sink.records() == operation_count &&
        spd_format_single_sink->records() == operation_count &&
        micro_format_multi_sink.records() == operation_count &&
        spd_format_multi_sink->records() == operation_count &&
        micro_format_single_sink.bytes() == spd_format_single_sink->bytes() &&
        micro_format_multi_sink.bytes() == spd_format_multi_sink->bytes() &&
        optimized_output.good() && legacy_output.good() &&
        optimized_multi_output.good() && legacy_multi_output.good() &&
        buffered_stream_output.good() && buffered_multi_output.good();
    if (!valid)
    {
        std::cerr << "logger benchmark validation failed\n";
        return 1;
    }

    std::cout << "comparison=MicroErrorSystem_vs_spdlog"
              << " operations=" << operation_count
              << " workers=" << worker_count << '\n';
    print_result(
        "dispatch_micro_single",
        micro_dispatch_single_sink.records(),
        micro_dispatch_single_us);
    print_result(
        "dispatch_spdlog_single",
        spd_dispatch_single_sink->records(),
        spd_dispatch_single_us);
    print_result(
        "dispatch_micro_multi",
        micro_dispatch_multi_sink.records(),
        micro_dispatch_multi_us);
    print_result(
        "dispatch_spdlog_multi",
        spd_dispatch_multi_sink->records(),
        spd_dispatch_multi_us);
    print_result(
        "dispatch_micro_multi_sharded",
        micro_sharded_sink.records(),
        micro_sharded_us);
    print_result(
        "dispatch_micro_multi_partitioned",
        partitioned_records,
        micro_partitioned_us);
    print_result(
        "format_micro_single",
        micro_format_single_sink.records(),
        micro_format_single_us,
        micro_format_single_sink.bytes());
    print_result(
        "format_spdlog_single",
        spd_format_single_sink->records(),
        spd_format_single_us,
        spd_format_single_sink->bytes());
    print_result(
        "format_micro_multi",
        micro_format_multi_sink.records(),
        micro_format_multi_us,
        micro_format_multi_sink.bytes());
    print_result(
        "format_spdlog_multi",
        spd_format_multi_sink->records(),
        spd_format_multi_us,
        spd_format_multi_sink->bytes());
    print_result("console_immediate_single", operation_count, optimized_console_us);
    print_result("console_legacy_single", operation_count, legacy_console_us);
    print_result("console_immediate_multi", operation_count, optimized_multi_console_us);
    print_result("buffered_stream_single", operation_count, buffered_stream_single_us);
    print_result("buffered_stream_multi", operation_count, buffered_multi_us);
    print_result("console_legacy_multi", operation_count, legacy_multi_console_us);

    return 0;
}
