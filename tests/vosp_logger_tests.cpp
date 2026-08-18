#include <vosp.hpp>

#include <atomic>
#include <iostream>
#include <sstream>
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

    /** @brief Verifies ConsoleSink formatting and stream error propagation. */
    bool test_console_sink()
    {
        std::ostringstream output;
        ConsoleSink sink{output};
        Logger logger{sink};

        return check(logger.error(Error{Category::FILESYSTEM, 3001, "disk full"}),
                     "console sink write") &&
               check(output.str() ==
                         "[ERROR] [FILESYSTEM] code=3001 message=disk full\n",
                     "console sink format");
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
}

int main()
{
    return test_string_conversion() &&
           test_logger_lifecycle() &&
           test_console_sink() &&
           test_logger_policy() &&
           test_reentrant_sink() &&
           test_concurrent_logging() &&
           test_parallel_logging() ? 0 : 1;
}
