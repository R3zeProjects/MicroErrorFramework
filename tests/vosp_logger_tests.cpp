#include <vosp/logger.hpp>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
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

    /** @brief In-memory sink used to verify logger delivery. */
    class TestSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry& entry) override
        {
            entries.push_back(entry);
            return true;
        }

        std::vector<LogEntry> entries;
    };

    /** @brief Thread-safe sink used to verify parallel dispatch. */
    class ConcurrentTestSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            writes_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        [[nodiscard]] std::uint32_t writes() const noexcept
        {
            return writes_.load(std::memory_order_relaxed);
        }

    private:
        std::atomic<std::uint32_t> writes_ = 0;
    };

    /** @brief Sink that observes delivery and reports a recoverable failure. */
    class RejectingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            ++writes;
            return false;
        }

        std::size_t writes = 0;
    };

    /** @brief Sink used to verify that capture preserves the operation error. */
    class ThrowingSink final : public ILogSink
    {
    public:
        [[nodiscard]] bool write(const LogEntry&) override
        {
            throw std::runtime_error{"sink failure"};
        }
    };

    /** @brief Sink that mutates logger membership while handling a record. */
    class ReentrantSink final : public ILogSink
    {
    public:
        ILogger* logger = nullptr;
        ILogSink* sink_to_attach = nullptr;

        [[nodiscard]] bool write(const LogEntry&) override
        {
            if (logger != nullptr && sink_to_attach != nullptr)
            {
                (void)logger->attach(*sink_to_attach);
                logger = nullptr;
            }

            return true;
        }
    };

    /** @brief Verifies level/category string conversion. */
    bool test_string_conversion()
    {
        return check(to_string(Level::WARNING) == "WARNING", "level conversion") &&
               check(to_string(Category::DATABASE) == "DATABASE", "category conversion");
    }

    /** @brief Verifies sink registration, broadcast, and removal. */
    bool test_logger_lifecycle()
    {
        TestSink sink;
        Logger logger{sink};
        ILogger& logging_service = logger;
        const Error error{Category::NETWORK, 1001, "connection refused"};

        return check(!logger.attach(sink), "duplicate sink") &&
               check(logging_service.critical(error), "polymorphic logger call") &&
               check(sink.entries.size() == 1, "sink receives one entry") &&
               check(sink.entries.front().level == Level::CRITICAL, "entry level") &&
               check(sink.entries.front().error == error, "entry error") &&
               check(logger.detach(sink), "detach sink") &&
               check(!logger.error(error), "logger without sinks");
    }

    /** @brief Verifies direct Result logging and exception capture. */
    bool test_error_observability()
    {
        TestSink sink;
        Logger logger{sink};
        const Error context{Category::DATABASE, 1100, "database operation"};

        const auto success = logger.capture(context, [] { return 12; });
        const auto failure = logger.capture(context, []() -> int
        {
            throw std::runtime_error{"timeout"};
        });
        const Result<int> result_failure{
            std::unexpect,
            Error{Category::NETWORK, 1101, "network result"}
        };
        const Result<int> result_success{1};

        ThrowingSink throwing_sink;
        Logger failing_logger{throwing_sink};
        const auto preserved = failing_logger.capture(context, []() -> int
        {
            throw std::runtime_error{"primary failure"};
        });

        return check(success && *success == 12, "capture returns successful value") &&
               check(!failure && failure.error().message() ==
                         "database operation: timeout",
                     "capture converts exception") &&
               check(sink.entries.size() == 1 &&
                         sink.entries.front().error == failure.error(),
                     "capture logs converted error") &&
               check(logger.error(result_success), "successful Result needs no log") &&
               check(logger.error(result_failure), "failed Result is accepted") &&
               check(sink.entries.size() == 2 &&
                         sink.entries.back().error == result_failure.error(),
                     "failed Result is logged directly") &&
               check(!preserved && preserved.error().message() ==
                         "database operation: primary failure",
                     "sink exception does not replace operation error");
    }

    /** @brief Verifies ConsoleSink formatting and stream error propagation. */
    bool test_console_sink()
    {
        std::ostringstream output;
        ConsoleSink sink{output};
        Logger logger{sink};

        const std::string long_message(1024, 'x');
        std::ostringstream long_output;
        ConsoleSink long_sink{long_output};
        Logger long_logger{long_sink};

        std::ostringstream failed_output;
        failed_output.setstate(std::ios::badbit);
        ConsoleSink failed_sink{failed_output};
        Logger failed_logger{failed_sink};

        return check(logger.error(Error{Category::FILESYSTEM, 3001, "disk full"}),
                     "console sink write") &&
               check(output.str() ==
                         "[ERROR] [FILESYSTEM] code=3001 message=disk full\n",
                     "console sink format") &&
               check(long_logger.info(Error{Category::NETWORK, 3002, long_message}),
                     "console sink long write") &&
               check(long_output.str() ==
                         "[INFO] [NETWORK] code=3002 message=" + long_message + "\n",
                     "console sink long format") &&
               check(!failed_logger.error(Error{Category::FILESYSTEM, 3003, "failure"}),
                     "console sink reports stream failure");
    }

    /** @brief Verifies buffered delivery, concurrent producers, and flush errors. */
    bool test_buffered_stream_sink()
    {
        std::ostringstream output;
        BufferedStreamSink sink{output, 4096};
        ParallelLogger logger{sink};

        if (!check(logger.info(Error{Category::NETWORK, 3100, "buffered"}),
                   "buffered sink accepts record") ||
            !check(output.str().empty(), "buffered sink delays stream output") ||
            !check(sink.flush(), "buffered sink flush") ||
            !check(output.str() == "[INFO] [NETWORK] code=3100 message=buffered\n",
                   "buffered sink format") ||
            !check(sink.flush_threshold() == 4096, "buffered sink threshold"))
        {
            return false;
        }

        constexpr std::size_t worker_count = 4;
        constexpr std::size_t records_per_worker = 128;
        std::ostringstream concurrent_output;
        BufferedStreamSink concurrent_sink{concurrent_output, 256};
        ParallelLogger concurrent_logger{concurrent_sink};
        std::vector<std::jthread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker)
        {
            workers.emplace_back([&concurrent_logger, worker]
            {
                for (std::size_t record = 0; record < records_per_worker; ++record)
                {
                    static_cast<void>(concurrent_logger.info(Error{
                        Category::NETWORK,
                        static_cast<std::uint32_t>(worker * records_per_worker + record),
                        "parallel buffered"}));
                }
            });
        }
        workers.clear();

        if (!check(concurrent_sink.flush(), "concurrent buffered flush"))
        {
            return false;
        }
        const auto text = concurrent_output.str();
        if (!check(
                static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) ==
                    worker_count * records_per_worker,
                "concurrent buffered record count"))
        {
            return false;
        }

        std::ostringstream failed_output;
        BufferedStreamSink failed_sink{failed_output};
        ParallelLogger failed_logger{failed_sink};
        if (!check(failed_logger.error(Error{Category::FILESYSTEM, 3101, "failure"}),
                   "failed buffered stream accepts record"))
        {
            return false;
        }
        failed_output.setstate(std::ios::badbit);
        if (!check(!failed_sink.flush(), "buffered sink reports flush failure"))
        {
            return false;
        }

        for (std::uint32_t iteration = 0; iteration < 16; ++iteration)
        {
            std::ostringstream lifecycle_output;
            BufferedStreamSink lifecycle_sink{lifecycle_output};
            ParallelLogger lifecycle_logger{lifecycle_sink};
            if (!check(
                    lifecycle_logger.info(
                        Error{Category::NETWORK, iteration, "short-lived sink"}) &&
                        lifecycle_sink.flush(),
                    "buffered sink repeated lifecycle"))
            {
                return false;
            }
        }

        try
        {
            BufferedStreamSink invalid_sink{output, 0};
            static_cast<void>(invalid_sink);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        return check(false, "buffered sink rejects zero threshold");
    }

    /** @brief Verifies compile-time logger filtering policy. */
    bool test_logger_policy()
    {
        TestSink sink;
        PolicyLogger<MinimumLevelPolicy<Level::WARNING>> logger{sink};
        const Error error{Category::DATABASE, 3002, "filtered"};

        return check(logger.info(error), "filtered log is accepted") &&
               check(sink.entries.empty(), "below-threshold record is dropped") &&
               check(logger.warning(error), "warning passes policy") &&
               check(sink.entries.size() == 1, "policy logger delivery");
    }

    /** @brief Verifies that the latency policy omits optional metadata. */
    bool test_minimal_metadata_policy()
    {
        TestSink sink;
        PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch, MinimalMetadataPolicy> logger{sink};

        return check(
                   logger.info(Error{Category::DATABASE, 3005, "minimal metadata"}),
                   "minimal metadata write") &&
               check(sink.entries.size() == 1, "minimal metadata delivery") &&
               check(
                   sink.entries.front().timestamp == std::chrono::system_clock::time_point{},
                   "minimal timestamp") &&
               check(sink.entries.front().thread_id == std::thread::id{}, "minimal thread id");
    }

    /** @brief Verifies sink callbacks do not run under the logger mutex. */
    bool test_reentrant_sink()
    {
        ReentrantSink reentrant;
        TestSink secondary;
        Logger logger{reentrant};
        reentrant.logger = &logger;
        reentrant.sink_to_attach = &secondary;

        return check(logger.info(Error{Category::NETWORK, 3003, "reentrant"}),
                     "reentrant sink write") &&
               check(logger.info(Error{Category::NETWORK, 3004, "second"}),
                     "logger remains usable after reentrant attach") &&
               check(secondary.entries.size() == 1, "reentrant sink attached");
    }

    /** @brief Verifies serialized logger access from multiple threads. */
    bool test_concurrent_logging()
    {
        TestSink sink;
        Logger logger{sink};
        std::vector<std::thread> workers;

        for (std::uint32_t code = 0; code < 16; ++code)
        {
            workers.emplace_back([&logger, code]
            {
                (void)logger.warning(Error{Category::NETWORK, code, "parallel"});
            });
        }

        for (auto& worker : workers)
        {
            worker.join();
        }

        return check(sink.entries.size() == 16, "concurrent logging count");
    }

    /** @brief Verifies opt-in parallel dispatch with a thread-safe sink. */
    bool test_parallel_logging()
    {
        constexpr std::uint32_t worker_count = 4;
        constexpr std::uint32_t records_per_worker = 64;
        ConcurrentTestSink sink;
        ParallelLogger logger{sink};
        std::vector<std::thread> workers;
        workers.reserve(worker_count);

        for (std::uint32_t worker = 0; worker < worker_count; ++worker)
        {
            workers.emplace_back([&logger, worker]
            {
                for (std::uint32_t record = 0; record < records_per_worker; ++record)
                {
                    (void)logger.info(Error{
                        Category::NETWORK,
                        worker * records_per_worker + record,
                        "parallel"});
                }
            });
        }

        for (auto& worker : workers)
        {
            worker.join();
        }

        return check(
            sink.writes() == worker_count * records_per_worker,
            "parallel logging count");
    }

    /** @brief Verifies that a logger can retain an owned sink safely. */
    bool test_owned_sink_lifetime()
    {
        Logger logger;
        auto sink = std::make_shared<ConcurrentTestSink>();
        const std::weak_ptr<ConcurrentTestSink> weak_sink = sink;

        if (!check(logger.attach(sink), "attach owned sink"))
        {
            return false;
        }

        sink.reset();
        auto retained_sink = weak_sink.lock();
        if (!check(static_cast<bool>(retained_sink), "logger retains sink"))
        {
            return false;
        }

        if (!check(logger.info(Error{Category::NETWORK, 4001, "owned"}), "owned sink write") ||
            !check(retained_sink->writes() == 1, "owned sink receives entry") ||
            !check(logger.detach(*retained_sink), "detach owned sink"))
        {
            return false;
        }

        retained_sink.reset();
        return check(weak_sink.expired(), "owned sink is released after detach");
    }

    /** @brief Verifies null ownership and multi-sink failure aggregation. */
    bool test_sink_failure_contract()
    {
        Logger logger;
        TestSink accepted_sink;
        TestSink unknown_sink;
        RejectingSink rejected_sink;
        std::shared_ptr<ILogSink> null_sink;

        if (!check(!logger.attach(std::move(null_sink)), "null owned sink is rejected") ||
            !check(logger.attach(accepted_sink), "first sink is attached") ||
            !check(logger.attach(rejected_sink), "second sink is attached") ||
            !check(!logger.attach(accepted_sink), "duplicate sink is rejected"))
        {
            return false;
        }

        const bool accepted = logger.error(
            Error{Category::DATABASE, 4002, "partial sink failure"});
        return check(!accepted, "logger aggregates sink rejection") &&
               check(accepted_sink.entries.size() == 1,
                     "successful sink still receives the record") &&
               check(rejected_sink.writes == 1,
                     "rejecting sink receives the record") &&
               check(!logger.detach(unknown_sink),
                     "detaching an unknown sink is rejected");
    }

    /** @brief Verifies shared sink ownership across asynchronous delivery. */
    bool test_async_owned_sink_lifetime()
    {
        using AsyncLogger =
            PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy>;

        AsyncLogger logger;
        auto sink = std::make_shared<ConcurrentTestSink>();
        const std::weak_ptr<ConcurrentTestSink> weak_sink = sink;
        if (!check(logger.attach(sink), "attach async owned sink"))
        {
            return false;
        }

        sink.reset();
        auto retained_sink = weak_sink.lock();
        if (!check(static_cast<bool>(retained_sink), "async logger retains sink") ||
            !check(logger.info(Error{Category::NETWORK, 4003, "async owned"}),
                   "async owned sink accepts record"))
        {
            return false;
        }

        logger.flush();
        if (!check(retained_sink->writes() == 1, "async owned sink receives record") ||
            !check(logger.detach(*retained_sink), "detach async owned sink"))
        {
            return false;
        }

        retained_sink.reset();
        logger.shutdown();
        return check(weak_sink.expired(), "async owned sink is released after detach");
    }

    /** @brief Verifies bounded asynchronous delivery and flush semantics. */
    bool test_async_logger_policy()
    {
        ConcurrentTestSink sink;
        PolicyLogger<AcceptAllPolicy, AsyncSinkDispatch, MinimalMetadataPolicy> logger{sink};

        for (std::uint32_t code = 0; code < 256; ++code)
        {
            if (!logger.info(Error{Category::NETWORK, code, "async owned record"}))
            {
                return check(false, "async logger accepts record");
            }
        }

        logger.flush();
        return check(sink.writes() == 256, "async logger delivery") &&
               check(logger.failed_records() == 0, "async logger sink failures");
    }

}

int main()
{
    return test_string_conversion() &&
           test_logger_lifecycle() &&
           test_error_observability() &&
           test_console_sink() &&
           test_buffered_stream_sink() &&
           test_logger_policy() &&
           test_minimal_metadata_policy() &&
           test_reentrant_sink() &&
           test_concurrent_logging() &&
           test_parallel_logging() &&
           test_owned_sink_lifetime() &&
           test_sink_failure_contract() &&
           test_async_owned_sink_lifetime() &&
           test_async_logger_policy() ? 0 : 1;
}
