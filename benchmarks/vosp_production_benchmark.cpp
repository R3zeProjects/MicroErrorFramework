#include <vosp.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace allocation_probe
{
    inline std::atomic<bool> enabled = false;
    inline std::atomic<std::uint64_t> count = 0;
    inline std::atomic<std::uint64_t> bytes = 0;

    void account(std::size_t size) noexcept
    {
        if (enabled.load(std::memory_order_relaxed))
        {
            count.fetch_add(1, std::memory_order_relaxed);
            bytes.fetch_add(size, std::memory_order_relaxed);
        }
    }
}

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define VOSP_BENCHMARK_ADDRESS_SANITIZER 1
#  endif
#endif

#if !defined(VOSP_BENCHMARK_ADDRESS_SANITIZER) && !defined(__SANITIZE_ADDRESS__)
void* operator new(std::size_t size)
{
    if (void* pointer = std::malloc(size))
    {
        allocation_probe::account(size);
        return pointer;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (void* pointer = std::malloc(size))
    {
        allocation_probe::account(size);
        return pointer;
    }
    throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    using namespace vosp::error;
    using namespace vosp::logger;
    using vosp::async::IndustrialWorkerPool;
    using vosp::async::ShutdownMode;

    struct Configuration
    {
        enum class Profile { QUICK, FULL, SOAK } profile = Profile::QUICK;
        enum class Suite { ALL, LOGGER, WORKER, REGISTER } suite = Suite::ALL;
        std::size_t operations = 20'000;
        std::chrono::seconds soak_duration{30};
        std::optional<std::string> csv_path;
    };

    struct Statistics
    {
        std::uint64_t elapsed_ns = 0;
        std::uint64_t accepted = 0;
        std::uint64_t failed = 0;
        std::vector<std::uint64_t> samples;
    };

    struct AllocationResult
    {
        std::uint64_t count = 0;
        std::uint64_t bytes = 0;
    };

    class AllocationScope
    {
    public:
        AllocationScope()
        {
            allocation_probe::count.store(0, std::memory_order_relaxed);
            allocation_probe::bytes.store(0, std::memory_order_relaxed);
            allocation_probe::enabled.store(true, std::memory_order_release);
        }

        AllocationScope(const AllocationScope&) = delete;
        AllocationScope& operator=(const AllocationScope&) = delete;

        [[nodiscard]] AllocationResult finish() noexcept
        {
            allocation_probe::enabled.store(false, std::memory_order_release);
            return {
                allocation_probe::count.load(std::memory_order_relaxed),
                allocation_probe::bytes.load(std::memory_order_relaxed)
            };
        }

        ~AllocationScope() { allocation_probe::enabled.store(false, std::memory_order_release); }
    };

    [[nodiscard]] std::uint64_t percentile(
        const std::vector<std::uint64_t>& sorted,
        double fraction)
    {
        if (sorted.empty()) return 0;
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    }

    class Reporter
    {
    public:
        explicit Reporter(const std::optional<std::string>& path)
        {
            if (path)
            {
                file_.open(*path, std::ios::trunc);
                if (!file_) throw std::runtime_error{"Cannot open benchmark CSV output"};
            }
            write_header(std::cout);
            if (file_) write_header(file_);
        }

        void report(
            std::string_view subsystem,
            std::string_view scenario,
            std::size_t producers,
            std::size_t workers,
            std::size_t message_bytes,
            std::size_t operations,
            Statistics statistics,
            AllocationResult allocations = {},
            std::string_view notes = {})
        {
            std::ranges::sort(statistics.samples);
            write(std::cout, subsystem, scenario, producers, workers, message_bytes,
                  operations, statistics, allocations, notes);
            if (file_)
            {
                write(file_, subsystem, scenario, producers, workers, message_bytes,
                      operations, statistics, allocations, notes);
                file_.flush();
            }
        }

    private:
        static void write_header(std::ostream& output)
        {
            output << "subsystem,scenario,producers,workers,message_bytes,operations,"
                      "elapsed_ms,throughput_per_second,p50_ns,p95_ns,p99_ns,max_ns,"
                      "accepted,failed,allocations,allocated_bytes,notes\n";
        }

        static void write(
            std::ostream& output,
            std::string_view subsystem,
            std::string_view scenario,
            std::size_t producers,
            std::size_t workers,
            std::size_t message_bytes,
            std::size_t operations,
            const Statistics& statistics,
            const AllocationResult& allocations,
            std::string_view notes)
        {
            const auto seconds = static_cast<double>(statistics.elapsed_ns) / 1e9;
            const auto throughput = seconds > 0.0
                ? static_cast<double>(operations) / seconds : 0.0;
            output << subsystem << ',' << scenario << ',' << producers << ',' << workers
                   << ',' << message_bytes << ',' << operations << ','
                   << static_cast<double>(statistics.elapsed_ns) / 1e6 << ',' << throughput
                   << ',' << percentile(statistics.samples, 0.50)
                   << ',' << percentile(statistics.samples, 0.95)
                   << ',' << percentile(statistics.samples, 0.99)
                   << ',' << (statistics.samples.empty() ? 0 : statistics.samples.back())
                   << ',' << statistics.accepted << ',' << statistics.failed
                   << ',' << allocations.count << ',' << allocations.bytes
                   << ",\"" << notes << "\"\n";
        }

        std::ofstream file_;
    };

    template<typename Operation>
    [[nodiscard]] Statistics run_producers(
        std::size_t producer_count,
        std::size_t operation_count,
        Operation&& operation,
        std::size_t maximum_samples = 4096)
    {
        std::latch ready{static_cast<std::ptrdiff_t>(producer_count)};
        std::latch start{1};
        std::vector<std::jthread> producers;
        std::vector<std::vector<std::uint64_t>> samples(producer_count);
        std::vector<std::uint64_t> accepted(producer_count, 0);
        std::vector<std::uint64_t> failed(producer_count, 0);
        producers.reserve(producer_count);
        const auto stride = std::max<std::size_t>(1, operation_count / maximum_samples);

        for (std::size_t producer = 0; producer < producer_count; ++producer)
        {
            producers.emplace_back([&, producer]
            {
                const auto begin = operation_count * producer / producer_count;
                const auto end = operation_count * (producer + 1) / producer_count;
                samples[producer].reserve((end - begin) / stride + 1);
                ready.count_down();
                start.wait();
                for (std::size_t index = begin; index < end; ++index)
                {
                    const bool sampled = index % stride == 0;
                    const auto before = sampled ? Clock::now() : Clock::time_point{};
                    if (operation(producer, index)) ++accepted[producer];
                    else ++failed[producer];
                    if (sampled)
                    {
                        samples[producer].push_back(static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                Clock::now() - before).count()));
                    }
                }
            });
        }

        ready.wait();
        const auto before = Clock::now();
        start.count_down();
        producers.clear();
        const auto elapsed = Clock::now() - before;

        Statistics result;
        result.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        result.accepted = std::accumulate(accepted.begin(), accepted.end(), std::uint64_t{});
        result.failed = std::accumulate(failed.begin(), failed.end(), std::uint64_t{});
        for (auto& producer_samples : samples)
        {
            result.samples.insert(result.samples.end(),
                                  producer_samples.begin(), producer_samples.end());
        }
        return result;
    }

    class CountingSink final : public ILogSink
    {
    public:
        explicit CountingSink(std::chrono::nanoseconds delay = {}, std::size_t fail_every = 0)
            : delay_{delay}, fail_every_{fail_every} {}

        [[nodiscard]] bool write(const LogEntry&) override
        {
            if (delay_ != std::chrono::nanoseconds{})
            {
                const auto until = Clock::now() + delay_;
                while (Clock::now() < until) std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            const auto sequence = records_.fetch_add(1, std::memory_order_relaxed) + 1;
            return fail_every_ == 0 || sequence % fail_every_ != 0;
        }

        [[nodiscard]] std::uint64_t records() const noexcept
        {
            return records_.load(std::memory_order_relaxed);
        }

    private:
        std::chrono::nanoseconds delay_;
        std::size_t fail_every_;
        std::atomic<std::uint64_t> records_ = 0;
    };

    class ShardedSink final : public ILogSink
    {
        struct alignas(64) Counter { std::uint64_t value = 0; };
        struct Slot { std::uint64_t generation = 0; std::size_t index = 0; };
    public:
        ShardedSink() : generation_{next_generation_.fetch_add(1, std::memory_order_relaxed)} {}

        [[nodiscard]] bool write(const LogEntry&) noexcept override
        {
            thread_local Slot slot;
            if (slot.generation != generation_)
            {
                slot = {generation_, next_slot_.fetch_add(1, std::memory_order_relaxed)};
            }
            if (slot.index < counters_.size()) ++counters_[slot.index].value;
            else overflow_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] std::uint64_t records() const noexcept
        {
            auto total = overflow_.load(std::memory_order_relaxed);
            for (const auto& counter : counters_) total += counter.value;
            return total;
        }

    private:
        inline static std::atomic<std::uint64_t> next_generation_ = 1;
        const std::uint64_t generation_;
        std::atomic<std::size_t> next_slot_ = 0;
        std::array<Counter, 64> counters_{};
        std::atomic<std::uint64_t> overflow_ = 0;
    };

    class NullBuffer final : public std::streambuf
    {
    public:
        [[nodiscard]] std::uint64_t bytes() const noexcept
        {
            return bytes_.load(std::memory_order_relaxed);
        }

    private:
        std::streamsize xsputn(const char*, std::streamsize count) override
        {
            bytes_.fetch_add(static_cast<std::uint64_t>(count), std::memory_order_relaxed);
            return count;
        }

        int_type overflow(int_type value) override
        {
            if (!traits_type::eq_int_type(value, traits_type::eof()))
            {
                bytes_.fetch_add(1, std::memory_order_relaxed);
            }
            return traits_type::not_eof(value);
        }

        std::atomic<std::uint64_t> bytes_ = 0;
    };

    [[nodiscard]] std::string message(std::size_t size)
    {
        return std::string(size, 'x');
    }

    template<typename LoggerType, typename Sink>
    void benchmark_logger(
        Reporter& reporter,
        std::string_view scenario,
        Sink& sink,
        std::size_t producers,
        std::size_t operations,
        std::size_t message_size)
    {
        LoggerType logger{sink};
        const Error prepared{Category::NETWORK, 1001, message(message_size)};
        auto stats = run_producers(producers, operations,
            [&](std::size_t, std::size_t) { return logger.info(prepared); });
        const auto records = sink.records();
        if (records != stats.accepted) throw std::runtime_error{"logger record mismatch"};
        reporter.report("logger", scenario, producers, 0, message_size, operations,
                        std::move(stats), {}, "prepared owning Error");
    }

    void logger_suite(Reporter& reporter, const Configuration& config)
    {
        const std::array<std::size_t, 3> quick_producers{1, 4, 16};
        const std::array<std::size_t, 5> full_producers{1, 2, 4, 8, 16};
        const std::array<std::size_t, 3> quick_messages{16, 256, 4096};
        const std::array<std::size_t, 4> full_messages{16, 128, 1024, 8192};
        const auto producers = config.profile == Configuration::Profile::FULL
            ? std::span<const std::size_t>{full_producers}
            : std::span<const std::size_t>{quick_producers};
        const auto messages = config.profile == Configuration::Profile::FULL
            ? std::span<const std::size_t>{full_messages}
            : std::span<const std::size_t>{quick_messages};

        using FastLogger = PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch,
                                        MinimalMetadataPolicy>;
        using SafeLogger = PolicyLogger<AcceptAllPolicy, SerializedSinkDispatch,
                                        FullMetadataPolicy>;
        for (const auto message_size : messages)
        {
            for (const auto producer_count : producers)
            {
                ShardedSink sink;
                benchmark_logger<FastLogger>(reporter, "parallel_sharded", sink,
                    producer_count, config.operations, message_size);
            }
        }

        for (const auto producer_count : producers)
        {
            CountingSink sink;
            benchmark_logger<SafeLogger>(reporter, "serialized_full_metadata", sink,
                producer_count, config.operations, 128);
        }

        for (const auto sink_count : std::array<std::size_t, 4>{1, 2, 4, 8})
        {
            std::vector<std::unique_ptr<CountingSink>> sinks;
            sinks.reserve(sink_count);
            FastLogger logger;
            for (std::size_t index = 0; index < sink_count; ++index)
            {
                sinks.push_back(std::make_unique<CountingSink>());
                if (!logger.attach(*sinks.back())) throw std::runtime_error{"fanout attach failed"};
            }
            const Error prepared{Category::NETWORK, 1005, message(128)};
            auto stats = run_producers(4, config.operations,
                [&](std::size_t, std::size_t) { return logger.info(prepared); });
            std::uint64_t delivered = 0;
            for (const auto& sink : sinks) delivered += sink->records();
            if (delivered != config.operations * sink_count)
                throw std::runtime_error{"fanout delivery mismatch"};
            reporter.report("logger", "sink_fanout_" + std::to_string(sink_count),
                            4, 0, 128, config.operations, std::move(stats), {},
                            "all sinks receive every record");
        }

        CountingSink rejecting_sink{{}, 100};
        FastLogger rejecting_logger{rejecting_sink};
        const Error rejected_error{Category::NETWORK, 1006, message(128)};
        auto rejection_stats = run_producers(4, config.operations,
            [&](std::size_t, std::size_t) { return rejecting_logger.info(rejected_error); });
        reporter.report("logger", "sink_rejection_1_percent", 4, 0, 128,
                        config.operations, std::move(rejection_stats), {},
                        "failed calls are propagated to producers");

        using FilteredLogger = PolicyLogger<MinimumLevelPolicy<Level::WARNING>,
                                            ParallelSinkDispatch, MinimalMetadataPolicy>;
        CountingSink filtered_sink;
        FilteredLogger filtered{filtered_sink};
        const Error filtered_error{Category::NETWORK, 1002, message(128)};
        auto filtered_stats = run_producers(4, config.operations,
            [&](std::size_t, std::size_t) { return filtered.debug(filtered_error); });
        reporter.report("logger", "compile_time_filter_drop", 4, 0, 128,
                        config.operations, std::move(filtered_stats), {}, "zero sink calls");

        NullBuffer null_buffer;
        std::ostream output{&null_buffer};
        for (const auto producer_count : producers)
        {
            ConsoleSink immediate{output};
            SafeLogger immediate_logger{immediate};
            const Error prepared{Category::NETWORK, 1003, message(128)};
            auto stats = run_producers(producer_count, config.operations,
                [&](std::size_t, std::size_t) { return immediate_logger.info(prepared); });
            reporter.report("sink", "immediate_stream", producer_count, 0, 128,
                            config.operations, std::move(stats));

            BufferedStreamSink buffered{output};
            FastLogger buffered_logger{buffered};
            auto buffered_stats = run_producers(producer_count, config.operations,
                [&](std::size_t, std::size_t) { return buffered_logger.info(prepared); });
            const auto flush_start = Clock::now();
            const auto flushed = buffered.flush();
            buffered_stats.elapsed_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - flush_start).count());
            if (!flushed) throw std::runtime_error{"buffered sink flush failed"};
            reporter.report("sink", "buffered_stream", producer_count, 0, 128,
                            config.operations, std::move(buffered_stats), {}, "includes flush");
        }

        for (const auto producer_count : producers)
        {
            for (const auto delay : {std::chrono::nanoseconds{0},
                                     std::chrono::nanoseconds{5'000}})
            {
                CountingSink sink{delay, delay.count() == 0 ? 0U : 100U};
                using AsyncLogger = PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch,
                                                 MinimalMetadataPolicy>;
                AsyncLogger logger{sink};
                const auto async_operations = delay.count() == 0
                    ? config.operations : std::min<std::size_t>(config.operations, 4'000);
                const Error prepared{Category::NETWORK, 1004, message(128)};
                auto stats = run_producers(producer_count, async_operations,
                    [&](std::size_t, std::size_t) { return logger.info(prepared); });
                const auto flush_start = Clock::now();
                logger.flush();
                stats.elapsed_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now() - flush_start).count());
                stats.failed = logger.failed_records();
                if (sink.records() != stats.accepted)
                    throw std::runtime_error{"async logger record mismatch"};
                reporter.report("logger", delay.count() == 0 ? "async" : "async_slow_sink",
                                producer_count, 1, 128, async_operations, std::move(stats), {},
                                delay.count() == 0 ? "includes flush" : "5us sink; 1% rejects");
            }
        }

        using EndToEndAsyncLogger = PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch,
                                                 MinimalMetadataPolicy>;
        for (const auto producer_count : producers)
        {
            NullBuffer end_to_end_buffer;
            std::ostream end_to_end_output{&end_to_end_buffer};
            BufferedStreamSink end_to_end_sink{end_to_end_output};
            EndToEndAsyncLogger end_to_end_logger{end_to_end_sink};
            const Error prepared{Category::NETWORK, 1007, message(128)};
            auto stats = run_producers(producer_count, config.operations,
                [&](std::size_t, std::size_t) { return end_to_end_logger.info(prepared); });
            const auto flush_start = Clock::now();
            end_to_end_logger.flush();
            const auto sink_flushed = end_to_end_sink.flush();
            stats.elapsed_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - flush_start).count());
            stats.failed = end_to_end_logger.failed_records();
            if (!sink_flushed || end_to_end_buffer.bytes() == 0 || stats.failed != 0)
            {
                throw std::runtime_error{"async end-to-end delivery mismatch"};
            }
            reporter.report("logger", "async_e2e_buffered_stream", producer_count, 1, 128,
                            config.operations, std::move(stats), {},
                            "producer->queue->format->buffered sink->flush");
        }

        CountingSink allocation_sink;
        EndToEndAsyncLogger allocation_logger{allocation_sink};
        const Error allocation_error{Category::NETWORK, 1008, message(128)};
        AllocationResult async_allocations;
        Statistics allocation_stats;
        {
            AllocationScope scope;
            allocation_stats = run_producers(1, config.operations,
                [&](std::size_t, std::size_t)
                {
                    return allocation_logger.info(allocation_error);
                });
            allocation_logger.flush();
            async_allocations = scope.finish();
        }
        reporter.report("memory", "async_logger_allocations", 1, 1, 128,
                        config.operations, std::move(allocation_stats), async_allocations,
                        "accepted record copies, queue growth and dispatch; includes flush");
    }

    void worker_suite(Reporter& reporter, const Configuration& config)
    {
        const std::array<std::size_t, 3> quick_workers{1, 4, 16};
        const std::array<std::size_t, 5> full_workers{1, 2, 4, 8, 16};
        const auto workers = config.profile == Configuration::Profile::FULL
            ? std::span<const std::size_t>{full_workers}
            : std::span<const std::size_t>{quick_workers};
        for (const auto worker_count : workers)
        {
            for (const auto queue_capacity : {std::size_t{64}, std::size_t{1024}})
            {
                for (const auto producer_count : {std::size_t{1}, std::size_t{4}})
                {
                    IndustrialWorkerPool pool{worker_count, queue_capacity};
                    std::atomic<std::uint64_t> completed = 0;
                    auto stats = run_producers(producer_count, config.operations,
                        [&](std::size_t, std::size_t)
                        {
                            pool.dispatch([&completed]() -> OperationResult
                            {
                                completed.fetch_add(1, std::memory_order_relaxed);
                                return {};
                            });
                            return true;
                        });
                    const auto wait_start = Clock::now();
                    pool.wait();
                    stats.elapsed_ns += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - wait_start).count());
                    if (completed.load(std::memory_order_relaxed) != config.operations)
                        throw std::runtime_error{"worker dispatch mismatch"};
                    reporter.report("worker", queue_capacity == 64 ? "dispatch_q64" : "dispatch_q1024",
                                    producer_count, worker_count, 0, config.operations,
                                    std::move(stats));
                }
            }
        }

        for (const auto delay : {std::chrono::nanoseconds{1'000},
                                 std::chrono::nanoseconds{20'000}})
        {
            IndustrialWorkerPool pool{4, 64};
            const auto operations = std::min<std::size_t>(config.operations, 10'000);
            std::atomic<std::uint64_t> completed = 0;
            auto stats = run_producers(8, operations,
                [&](std::size_t, std::size_t)
                {
                    pool.dispatch([&completed, delay]() -> OperationResult
                    {
                        const auto until = Clock::now() + delay;
                        while (Clock::now() < until)
                            std::atomic_signal_fence(std::memory_order_seq_cst);
                        completed.fetch_add(1, std::memory_order_relaxed);
                        return {};
                    });
                    return true;
                });
            const auto wait_start = Clock::now();
            pool.wait();
            stats.elapsed_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - wait_start).count());
            reporter.report("worker", delay.count() == 1'000 ? "backpressure_1us" : "backpressure_20us",
                            8, 4, 0, operations, std::move(stats), {}, "queue=64");
        }

        IndustrialWorkerPool tracked_pool{4, 1024};
        std::vector<std::future<OperationResult>> futures;
        futures.reserve(config.operations);
        const auto tracked_start = Clock::now();
        for (std::size_t index = 0; index < config.operations; ++index)
            futures.push_back(tracked_pool.submit([]() -> OperationResult { return {}; }));
        std::uint64_t accepted = 0;
        for (auto& future : futures) if (future.get()) ++accepted;
        Statistics tracked;
        tracked.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - tracked_start).count());
        tracked.accepted = accepted;
        reporter.report("worker", "tracked_future", 1, 4, 0, config.operations,
                        std::move(tracked));

        std::vector<IndustrialWorkerPool::Task> tasks;
        tasks.reserve(config.operations);
        std::atomic<std::uint64_t> bulk_completed = 0;
        for (std::size_t index = 0; index < config.operations; ++index)
            tasks.emplace_back([&bulk_completed]() -> OperationResult
            {
                bulk_completed.fetch_add(1, std::memory_order_relaxed);
                return {};
            });
        IndustrialWorkerPool bulk_pool{4, 1024};
        const auto bulk_start = Clock::now();
        const auto bulk_accepted = bulk_pool.dispatch_bulk(tasks);
        bulk_pool.wait();
        Statistics bulk;
        bulk.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - bulk_start).count());
        bulk.accepted = bulk_accepted;
        reporter.report("worker", "bulk", 1, 4, 0, config.operations, std::move(bulk));

        for (const auto iterations : {std::size_t{256}, std::size_t{4096}})
        {
            const auto cpu_operations = iterations == 256
                ? config.operations : std::min<std::size_t>(config.operations, 25'000);
            IndustrialWorkerPool cpu_pool{4, 1024};
            std::atomic<std::uint64_t> checksum = 0;
            auto cpu_stats = run_producers(4, cpu_operations,
                [&](std::size_t, std::size_t index)
                {
                    cpu_pool.dispatch([&checksum, iterations, index]() -> OperationResult
                    {
                        std::uint64_t value = index + 0x9e3779b97f4a7c15ULL;
                        for (std::size_t step = 0; step < iterations; ++step)
                            value = (value ^ (value >> 29U)) * 0xbf58476d1ce4e5b9ULL;
                        checksum.fetch_xor(value, std::memory_order_relaxed);
                        return {};
                    });
                    return true;
                });
            const auto wait_start = Clock::now();
            cpu_pool.wait();
            cpu_stats.elapsed_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - wait_start).count());
            std::atomic_signal_fence(std::memory_order_seq_cst);
            reporter.report("worker", iterations == 256 ? "cpu_256_iterations" : "cpu_4096_iterations",
                            4, 4, 0, cpu_operations, std::move(cpu_stats), {},
                            "checksum=" + std::to_string(checksum.load(std::memory_order_relaxed)));
        }

        IndustrialWorkerPool failure_pool{4, 1024};
        const auto failure_operations = std::min<std::size_t>(config.operations, 20'000);
        auto failure_stats = run_producers(4, failure_operations,
            [&](std::size_t, std::size_t index)
            {
                failure_pool.dispatch([index]() -> OperationResult
                {
                    if (index % 250 == 0) throw std::runtime_error{"benchmark task failure"};
                    if (index % 100 == 0)
                        return std::unexpected(Error{Category::NONE, 5001, "expected failure"});
                    return {};
                });
                return true;
            });
        const auto failure_wait = Clock::now();
        failure_pool.wait();
        failure_stats.elapsed_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - failure_wait).count());
        failure_stats.failed = failure_pool.failed_dispatches();
        reporter.report("worker", "failure_and_exception_accounting", 4, 4, 0,
                        failure_operations, std::move(failure_stats), {},
                        "result failures and exceptions are counted");

        IndustrialWorkerPool cancel_pool{1, 1024};
        std::vector<std::future<OperationResult>> cancelled_futures;
        cancelled_futures.reserve(512);
        for (std::size_t index = 0; index < 512; ++index)
        {
            cancelled_futures.push_back(cancel_pool.submit([]() -> OperationResult
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                return {};
            }));
        }
        const auto cancel_start = Clock::now();
        cancel_pool.shutdown(ShutdownMode::CANCEL_PENDING);
        std::uint64_t cancelled = 0;
        for (auto& future : cancelled_futures)
        {
            const auto result = future.get();
            if (!result && result.error().code() == vosp::async::task_cancelled_code) ++cancelled;
        }
        Statistics cancel_stats;
        cancel_stats.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - cancel_start).count());
        cancel_stats.accepted = 512 - cancelled;
        cancel_stats.failed = cancelled;
        reporter.report("worker", "shutdown_cancel_pending", 1, 1, 0, 512,
                        std::move(cancel_stats), {}, "failed=cancelled futures");

        IndustrialWorkerPool measured_queue{4, 1024};
        Statistics queue_memory_stats;
        queue_memory_stats.accepted = 1;
        reporter.report("memory", "worker_queue_reserved_storage", 1, 4, 0, 1,
                        std::move(queue_memory_stats),
                        AllocationResult{1, measured_queue.queue_storage_bytes()},
                        "fixed ring slots only; excludes worker stacks and callable heaps");
        measured_queue.shutdown(ShutdownMode::DRAIN);
    }

    void register_suite(Reporter& reporter, const Configuration& config)
    {
        const std::array<std::size_t, 5> producer_counts{1, 2, 4, 8, 16};
        for (const auto producers : producer_counts)
        {
            MemoryRegister<Category::NETWORK> storage{config.operations, config.operations};
            MultiThreadedSystem<decltype(storage)> system{storage};
            auto stats = run_producers(producers, config.operations,
                [&](std::size_t, std::size_t index)
                {
                    return static_cast<bool>(system.add(Error{
                        Category::NETWORK, static_cast<std::uint32_t>(index), "production"}));
                });
            if (storage.size() != config.operations)
                throw std::runtime_error{"register add mismatch"};
            reporter.report("register", "same_category_add", producers, 0, 10,
                            config.operations, std::move(stats));
        }

        MemoryRegister<Category::NETWORK> network{config.operations / 3 + 1};
        MemoryRegister<Category::DATABASE> database{config.operations / 3 + 1};
        MemoryRegister<Category::FILESYSTEM> filesystem{config.operations / 3 + 1};
        MultiThreadedSystem<decltype(network), decltype(database), decltype(filesystem)>
            partitioned{network, database, filesystem};
        constexpr std::array categories{
            Category::NETWORK, Category::DATABASE, Category::FILESYSTEM};
        auto partitioned_stats = run_producers(3, config.operations - config.operations % 3,
            [&](std::size_t producer, std::size_t index)
            {
                return static_cast<bool>(partitioned.add(Error{
                    categories[producer], static_cast<std::uint32_t>(index), "production"}));
            });
        reporter.report("register", "partitioned_categories", 3, 0, 10,
                        config.operations - config.operations % 3, std::move(partitioned_stats));

        const auto duplicate_operations = std::min<std::size_t>(config.operations, 50'000);
        MemoryRegister<Category::NETWORK> duplicate_storage{1, 1};
        const Error duplicate{Category::NETWORK, 7, "duplicate"};
        if (!duplicate_storage.add(duplicate)) throw std::runtime_error{"register seed failed"};
        auto duplicate_stats = run_producers(4, duplicate_operations,
            [&](std::size_t, std::size_t) { return !duplicate_storage.add(duplicate); });
        reporter.report("register", "duplicate_rejection", 4, 0, 9,
                        duplicate_operations, std::move(duplicate_stats), {}, "accepted=expected rejection");

        MemoryRegister<Category::NETWORK> lifecycle_storage{config.operations, config.operations};
        MultiThreadedSystem<decltype(lifecycle_storage)> lifecycle{lifecycle_storage};
        for (std::size_t index = 0; index < config.operations; ++index)
            static_cast<void>(lifecycle.add(Error{Category::NETWORK,
                static_cast<std::uint32_t>(index), "lifecycle"}));
        auto contains_stats = run_producers(8, config.operations,
            [&](std::size_t, std::size_t index)
            {
                return lifecycle_storage.contains(Error{Category::NETWORK,
                    static_cast<std::uint32_t>(index), "lifecycle"});
            });
        reporter.report("register", "contains_hit", 8, 0, 9, config.operations,
                        std::move(contains_stats));
        auto remove_stats = run_producers(8, config.operations,
            [&](std::size_t, std::size_t index)
            {
                return static_cast<bool>(lifecycle.remove(Error{Category::NETWORK,
                    static_cast<std::uint32_t>(index), "lifecycle"}));
            });
        if (lifecycle_storage.size() != 0) throw std::runtime_error{"register remove mismatch"};
        reporter.report("register", "remove", 8, 0, 9, config.operations,
                        std::move(remove_stats));

        MemoryRegister<Category::NETWORK> full_storage{1, 1};
        static_cast<void>(full_storage.add(Error{Category::NETWORK, 1, "full"}));
        auto capacity_stats = run_producers(4, duplicate_operations,
            [&](std::size_t, std::size_t index)
            {
                const auto result = full_storage.add(Error{Category::NETWORK,
                    static_cast<std::uint32_t>(index + 2), "full"});
                return !result && result.error().code() == register_capacity_error_code;
            });
        reporter.report("register", "capacity_rejection", 4, 0, 4,
                        duplicate_operations, std::move(capacity_stats), {},
                        "accepted=expected capacity error");

        MemoryRegister<Category::NETWORK> routing_storage{1};
        MultiThreadedSystem<decltype(routing_storage)> routing{routing_storage};
        auto missing_stats = run_producers(4, duplicate_operations,
            [&](std::size_t, std::size_t index)
            {
                const auto result = routing.add(Error{Category::DATABASE,
                    static_cast<std::uint32_t>(index), "missing"});
                return !result && result.error().code() == missing_register_code;
            });
        reporter.report("register", "missing_category", 4, 0, 7,
                        duplicate_operations, std::move(missing_stats), {},
                        "accepted=expected routing error");

        AllocationResult allocations;
        {
            MemoryRegister<Category::NETWORK> measured{config.operations, config.operations};
            AllocationScope scope;
            for (std::size_t index = 0; index < config.operations; ++index)
                static_cast<void>(measured.add(Error{Category::NETWORK,
                    static_cast<std::uint32_t>(index), message(1024)}));
            allocations = scope.finish();
        }
        Statistics memory_stats;
        memory_stats.accepted = config.operations;
        reporter.report("memory", "register_long_message_allocations", 1, 0, 1024,
                        config.operations, std::move(memory_stats), allocations,
                        "global new/new[] only; throughput not measured");

        Statistics error_size_stats;
        error_size_stats.accepted = 1;
        reporter.report("memory", "error_object_static_size", 1, 0, 0, 1,
                        std::move(error_size_stats), AllocationResult{1, sizeof(Error)},
                        "sizeof(Error); excludes owned message allocation");
        Statistics entry_size_stats;
        entry_size_stats.accepted = 1;
        reporter.report("memory", "log_entry_static_size", 1, 0, 0, 1,
                        std::move(entry_size_stats), AllocationResult{1, sizeof(LogEntry)},
                        "sizeof(LogEntry); excludes owned message allocation");
    }

    void soak_suite(Reporter& reporter, const Configuration& config)
    {
        const auto producers = std::clamp<std::size_t>(
            std::thread::hardware_concurrency(), 1, 16);
        ShardedSink sink;
        using SoakLogger = PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch,
                                        MinimalMetadataPolicy>;
        SoakLogger logger{sink};
        const Error error{Category::NETWORK, 9000, message(128)};
        std::atomic<bool> stop = false;
        std::atomic<std::uint64_t> operations = 0;
        std::atomic<std::uint64_t> failures = 0;
        std::vector<std::jthread> threads;
        const auto start = Clock::now();
        for (std::size_t producer = 0; producer < producers; ++producer)
        {
            threads.emplace_back([&]
            {
                std::uint64_t local = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    if (!logger.info(error)) ++failures;
                    ++local;
                }
                operations.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(config.soak_duration);
        stop.store(true, std::memory_order_relaxed);
        threads.clear();
        Statistics stats;
        stats.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        stats.accepted = operations.load(std::memory_order_relaxed);
        stats.failed = failures.load(std::memory_order_relaxed);
        if (sink.records() != stats.accepted) throw std::runtime_error{"soak record mismatch"};
        reporter.report("soak", "parallel_logger", producers, 0, 128,
                        stats.accepted, std::move(stats), {}, "continuous duration profile");

        IndustrialWorkerPool pool{std::min<std::size_t>(producers, 8), 1024};
        std::atomic<std::uint64_t> worker_completed = 0;
        stop.store(false, std::memory_order_relaxed);
        threads.clear();
        const auto worker_start = Clock::now();
        for (std::size_t producer = 0; producer < std::min<std::size_t>(producers, 4); ++producer)
        {
            threads.emplace_back([&]
            {
                while (!stop.load(std::memory_order_relaxed))
                    pool.dispatch([&worker_completed]() -> OperationResult
                    {
                        worker_completed.fetch_add(1, std::memory_order_relaxed);
                        return {};
                    });
            });
        }
        std::this_thread::sleep_for(config.soak_duration);
        stop.store(true, std::memory_order_relaxed);
        threads.clear();
        pool.wait();
        Statistics worker_stats;
        worker_stats.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - worker_start).count());
        worker_stats.accepted = worker_completed.load(std::memory_order_relaxed);
        reporter.report("soak", "worker_dispatch", std::min<std::size_t>(producers, 4),
                        std::min<std::size_t>(producers, 8), 0, worker_stats.accepted,
                        std::move(worker_stats), {}, "bounded queue continuous profile");

        MemoryRegister<Category::NETWORK> register_storage{1024, 1024};
        MultiThreadedSystem<decltype(register_storage)> register_system{register_storage};
        stop.store(false, std::memory_order_relaxed);
        operations.store(0, std::memory_order_relaxed);
        failures.store(0, std::memory_order_relaxed);
        threads.clear();
        const auto register_start = Clock::now();
        for (std::size_t producer = 0; producer < std::min<std::size_t>(producers, 8); ++producer)
        {
            threads.emplace_back([&, producer]
            {
                const Error value{Category::NETWORK, static_cast<std::uint32_t>(producer), "soak"};
                std::uint64_t local = 0;
                while (!stop.load(std::memory_order_relaxed))
                {
                    if (!register_system.add(value)) ++failures;
                    if (!register_system.remove(value)) ++failures;
                    local += 2;
                }
                operations.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(config.soak_duration);
        stop.store(true, std::memory_order_relaxed);
        threads.clear();
        Statistics register_stats;
        register_stats.elapsed_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - register_start).count());
        register_stats.accepted = operations.load(std::memory_order_relaxed) -
                                  failures.load(std::memory_order_relaxed);
        register_stats.failed = failures.load(std::memory_order_relaxed);
        reporter.report("soak", "register_add_remove", std::min<std::size_t>(producers, 8),
                        0, 4, operations.load(std::memory_order_relaxed),
                        std::move(register_stats), {}, "continuous lifecycle profile");
    }

    [[nodiscard]] Configuration parse_arguments(int argc, char** argv)
    {
        Configuration config;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument{argv[index]};
            if (argument == "--profile=quick") config.profile = Configuration::Profile::QUICK;
            else if (argument == "--profile=full")
            {
                config.profile = Configuration::Profile::FULL;
                config.operations = 100'000;
            }
            else if (argument == "--profile=soak") config.profile = Configuration::Profile::SOAK;
            else if (argument.starts_with("--operations="))
                config.operations = std::stoull(std::string{argument.substr(13)});
            else if (argument == "--suite=all") config.suite = Configuration::Suite::ALL;
            else if (argument == "--suite=logger") config.suite = Configuration::Suite::LOGGER;
            else if (argument == "--suite=worker") config.suite = Configuration::Suite::WORKER;
            else if (argument == "--suite=register") config.suite = Configuration::Suite::REGISTER;
            else if (argument.starts_with("--duration="))
                config.soak_duration = std::chrono::seconds{
                    std::stoll(std::string{argument.substr(11)})};
            else if (argument.starts_with("--csv="))
                config.csv_path = std::string{argument.substr(6)};
            else throw std::invalid_argument{"Unknown benchmark argument: " + std::string{argument}};
        }
        if (config.operations == 0 || config.soak_duration.count() <= 0)
            throw std::invalid_argument{"operations and duration must be positive"};
        return config;
    }
}

int main(int argc, char** argv)
{
    try
    {
        const auto config = parse_arguments(argc, argv);
        Reporter reporter{config.csv_path};
        if (config.profile == Configuration::Profile::SOAK)
        {
            soak_suite(reporter, config);
            return 0;
        }
        if (config.suite == Configuration::Suite::ALL ||
            config.suite == Configuration::Suite::LOGGER)
            logger_suite(reporter, config);
        if (config.suite == Configuration::Suite::ALL ||
            config.suite == Configuration::Suite::WORKER)
            worker_suite(reporter, config);
        if (config.suite == Configuration::Suite::ALL ||
            config.suite == Configuration::Suite::REGISTER)
            register_suite(reporter, config);
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "production benchmark failed: " << exception.what() << '\n';
        return 1;
    }
}
