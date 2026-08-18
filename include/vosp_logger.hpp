#pragma once

#include "vosp_error.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <mutex>
#include <memory>
#include <ostream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace vosp::logger
{
    using vosp::error::Category;
    using vosp::error::Error;

    /** @brief Severity of a log record. */
    enum class Level : std::uint8_t
    {
        TRACE,
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        CRITICAL
    };

    /** @brief Default policy: publish every record. */
    struct AcceptAllPolicy
    {
        [[nodiscard]] static constexpr bool accepts(Level) noexcept { return true; }
    };

    /** @brief Compile-time policy that drops records below a minimum level. */
    template<Level Minimum>
    struct MinimumLevelPolicy
    {
        [[nodiscard]] static constexpr bool accepts(Level level) noexcept
        {
            return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(Minimum);
        }
    };

    template<typename Policy>
    concept LoggerPolicy = requires(Level level)
    {
        { Policy::accepts(level) } noexcept -> std::same_as<bool>;
    };

    /** @brief Dispatches sink callbacks one at a time for generic sink safety. */
    struct SerializedSinkDispatch
    {
        static constexpr bool serializes_callbacks = true;
    };

    /**
     * @brief Dispatches sink callbacks concurrently.
     * @warning Every attached sink must support concurrent calls to ILogSink::write.
     */
    struct ParallelSinkDispatch
    {
        static constexpr bool serializes_callbacks = false;
    };

    /** @brief Queues owned records and delivers them on one background worker. */
    struct AsyncSinkDispatch
    {
        static constexpr bool serializes_callbacks = false;
        static constexpr std::size_t queue_capacity = 1024;
    };

    /** @brief Restricts a type to a compile-time sink dispatch policy. */
    template<typename Dispatch>
    concept LoggerDispatchPolicy = requires
    {
        typename std::bool_constant<Dispatch::serializes_callbacks>;
    };

    /** @brief Captures timestamp and thread id for every record. */
    struct FullMetadataPolicy
    {
        [[nodiscard]] static std::chrono::system_clock::time_point timestamp() noexcept
        {
            return std::chrono::system_clock::now();
        }

        [[nodiscard]] static std::thread::id thread_id() noexcept
        {
            return std::this_thread::get_id();
        }
    };

    /** @brief Omits optional metadata on latency-sensitive logging paths. */
    struct MinimalMetadataPolicy
    {
        [[nodiscard]] static constexpr std::chrono::system_clock::time_point timestamp() noexcept
        {
            return {};
        }

        [[nodiscard]] static constexpr std::thread::id thread_id() noexcept
        {
            return {};
        }
    };

    template<typename Metadata>
    concept LoggerMetadataPolicy = requires
    {
        { Metadata::timestamp() } noexcept -> std::same_as<std::chrono::system_clock::time_point>;
        { Metadata::thread_id() } noexcept -> std::same_as<std::thread::id>;
    };

    /** @brief Converts a log level to a stable textual representation. */
    [[nodiscard]] constexpr std::string_view to_string(Level level) noexcept
    {
        switch (level)
        {
        case Level::TRACE: return "TRACE";
        case Level::DEBUG: return "DEBUG";
        case Level::INFO: return "INFO";
        case Level::WARNING: return "WARNING";
        case Level::ERROR: return "ERROR";
        case Level::CRITICAL: return "CRITICAL";
        }

        return "UNKNOWN";
    }

    /** @brief Converts an error category to a stable textual representation. */
    [[nodiscard]] constexpr std::string_view to_string(Category category) noexcept
    {
        switch (category)
        {
        case Category::NETWORK: return "NETWORK";
        case Category::DATABASE: return "DATABASE";
        case Category::FILESYSTEM: return "FILESYSTEM";
        case Category::NONE: return "NONE";
        }

        return "UNKNOWN";
    }

    /**
     * @brief Immutable data passed to log sinks.
     */
    struct LogEntry
    {
        std::chrono::system_clock::time_point timestamp;
        std::thread::id thread_id;
        Level level;
        Error error;
    };

    /**
     * @brief Destination for formatted or structured log records.
     * @note A sink attached by reference must outlive the Logger that references it.
     */
    class ILogSink
    {
    public:
        /**
         * @brief Consumes one log record.
         * @param entry Record to consume.
         * @return true when the sink accepted the record.
         */
        [[nodiscard]] virtual bool write(const LogEntry& entry) = 0;

        virtual ~ILogSink() noexcept = default;
    };

    /** @brief Restricts a type to concrete log sink implementations. */
    template<typename Sink>
    concept SinkType = std::derived_from<std::remove_cvref_t<Sink>, ILogSink>;

    /**
     * @brief Abstract logging service.
     */
    class ILogger
    {
    public:
        /** @brief Attaches a sink to the logging service. */
        [[nodiscard]] virtual bool attach(ILogSink& sink) = 0;

        /** @brief Detaches a sink from the logging service. */
        [[nodiscard]] virtual bool detach(ILogSink& sink) = 0;

        /** @brief Publishes a record at an explicit level. */
        [[nodiscard]] virtual bool write(Level level, const Error& error) = 0;

        [[nodiscard]] bool trace(const Error& error)
        {
            return write(Level::TRACE, error);
        }

        [[nodiscard]] bool trace(Error&& error)
        {
            return write_owned(Level::TRACE, std::move(error));
        }

        [[nodiscard]] bool debug(const Error& error)
        {
            return write(Level::DEBUG, error);
        }

        [[nodiscard]] bool debug(Error&& error)
        {
            return write_owned(Level::DEBUG, std::move(error));
        }

        [[nodiscard]] bool info(const Error& error)
        {
            return write(Level::INFO, error);
        }

        [[nodiscard]] bool info(Error&& error)
        {
            return write_owned(Level::INFO, std::move(error));
        }

        [[nodiscard]] bool warning(const Error& error)
        {
            return write(Level::WARNING, error);
        }

        [[nodiscard]] bool warning(Error&& error)
        {
            return write_owned(Level::WARNING, std::move(error));
        }

        [[nodiscard]] bool error(const Error& error_value)
        {
            return write(Level::ERROR, error_value);
        }

        [[nodiscard]] bool error(Error&& error_value)
        {
            return write_owned(Level::ERROR, std::move(error_value));
        }

        [[nodiscard]] bool critical(const Error& error)
        {
            return write(Level::CRITICAL, error);
        }

        [[nodiscard]] bool critical(Error&& error)
        {
            return write_owned(Level::CRITICAL, std::move(error));
        }

        /**
         * @brief Publishes an owned error without forcing a second message copy.
         * @note The default keeps compatibility for custom ILogger types.
         */
        [[nodiscard]] virtual bool write_owned(Level level, Error error)
        {
            return write(level, error);
        }

        virtual ~ILogger() noexcept = default;
    };

    /**
     * @brief Thread-safe logger that broadcasts records to external or owned sinks.
     */
    template<LoggerPolicy Policy = AcceptAllPolicy,
             LoggerDispatchPolicy Dispatch = SerializedSinkDispatch,
             LoggerMetadataPolicy Metadata = FullMetadataPolicy>
    class PolicyLogger final : public ILogger
    {
    private:
        struct SinkSlot
        {
            ILogSink* sink;
            std::shared_ptr<ILogSink> owner;
        };

    public:
        PolicyLogger() = default;

        /**
         * @brief Creates a logger with an initial set of sinks.
         * @param sinks Sink instances that must outlive this logger.
         */
        template<SinkType... Sinks>
        explicit PolicyLogger(Sinks&... sinks)
        {
            (static_cast<void>(attach(sinks)), ...);
        }

        /**
         * @brief Creates a logger that owns its initial sink.
         * @param sink Shared sink lifetime retained by this logger.
         */
        template<SinkType Sink>
        explicit PolicyLogger(std::shared_ptr<Sink> sink)
        {
            static_cast<void>(attach(std::move(sink)));
        }

        /**
         * @brief Adds a sink to the broadcast list.
         * @param sink Sink that must outlive this logger.
         * @return false when the sink is already registered.
         */
        [[nodiscard]] bool attach(ILogSink& sink) override
        {
            const std::lock_guard lock{mutex_};

            for (const auto& registered_sink : sinks_)
            {
                if (registered_sink.sink == &sink)
                {
                    return false;
                }
            }

            sinks_.push_back(SinkSlot{&sink, {}});
            refresh_single_sink_fast_path();
            return true;
        }

        /**
         * @brief Adds a sink and retains shared ownership of it.
         * @param sink Non-null sink whose lifetime is managed by shared ownership.
         * @return false when the pointer is null or the sink is already registered.
         */
        [[nodiscard]] bool attach(std::shared_ptr<ILogSink> sink)
        {
            if (!sink)
            {
                return false;
            }

            const std::lock_guard lock{mutex_};
            auto* const raw_sink = sink.get();
            for (const auto& registered_sink : sinks_)
            {
                if (registered_sink.sink == raw_sink)
                {
                    return false;
                }
            }

            sinks_.push_back(SinkSlot{raw_sink, std::move(sink)});
            refresh_single_sink_fast_path();
            return true;
        }

        /**
         * @brief Removes a sink from the broadcast list.
         * @param sink Sink to remove.
         * @return true when the sink was registered.
         */
        [[nodiscard]] bool detach(ILogSink& sink) override
        {
            const std::lock_guard lock{mutex_};

            const auto it = std::find_if(
                sinks_.begin(),
                sinks_.end(),
                [&sink](const auto& registered_sink)
                {
                    return registered_sink.sink == &sink;
                });

            if (it == sinks_.end())
            {
                return false;
            }

            sinks_.erase(it);
            refresh_single_sink_fast_path();
            return true;
        }

        /**
         * @brief Logs an error at ERROR level.
         * @param error Error to publish.
         * @return true when every registered sink accepted the record.
         */
        /**
         * @brief Publishes a record to every attached sink.
         * @param level Severity of the record.
         * @param error Error to publish.
         * @return true when every attached sink accepted the record.
         */
        [[nodiscard]] bool write(Level level, const Error& error) override
        {
            return write_entry(level, error);
        }

        [[nodiscard]] bool write_owned(Level level, Error error) override
        {
            return write_entry(level, std::move(error));
        }

    private:
        template<typename ErrorValue>
        [[nodiscard]] bool write_entry(Level level, ErrorValue&& error)
        {
            if (!Policy::accepts(level))
            {
                return true;
            }

            const LogEntry entry{
                Metadata::timestamp(),
                Metadata::thread_id(),
                level,
                std::forward<ErrorValue>(error)
            };

            // The common one-sink, externally-owned configuration does not
            // require a registry lock. The sink lifetime contract already
            // guarantees that an in-flight callback remains valid.
            if (auto* const sink = single_non_owning_sink_.load(std::memory_order_acquire))
            {
                if constexpr (Dispatch::serializes_callbacks)
                {
                    const std::lock_guard callback_lock{callback_mutex_};
                    return sink->write(entry);
                }

                return sink->write(entry);
            }

            std::vector<SinkSlot> sinks;
            ILogSink* single_sink = nullptr;
            [[maybe_unused]] std::shared_ptr<ILogSink> single_sink_owner;
            {
                const std::lock_guard lock{mutex_};

                if (sinks_.empty())
                {
                    return false;
                }

                if (sinks_.size() == 1)
                {
                    single_sink = sinks_.front().sink;
                    single_sink_owner = sinks_.front().owner;
                }
                else
                {
                    sinks = sinks_;
                }
            }

            if constexpr (Dispatch::serializes_callbacks)
            {
                const std::lock_guard callback_lock{callback_mutex_};
                return deliver(entry, single_sink, sinks);
            }

            return deliver(entry, single_sink, sinks);
        }

        [[nodiscard]] static bool deliver(
            const LogEntry& entry,
            ILogSink* single_sink,
            std::vector<SinkSlot>& sinks)
        {
            if (single_sink != nullptr)
            {
                return single_sink->write(entry);
            }

            bool accepted = true;
            for (const auto& sink : sinks)
            {
                accepted = sink.sink->write(entry) && accepted;
            }

            return accepted;
        }

        void refresh_single_sink_fast_path() noexcept
        {
            ILogSink* sink = nullptr;
            if (sinks_.size() == 1 && !sinks_.front().owner)
            {
                sink = sinks_.front().sink;
            }

            single_non_owning_sink_.store(sink, std::memory_order_release);
        }

        std::mutex mutex_;
        std::recursive_mutex callback_mutex_;
        std::vector<SinkSlot> sinks_;
        std::atomic<ILogSink*> single_non_owning_sink_ = nullptr;
    };

    /**
     * @brief Asynchronous specialization of PolicyLogger with bounded backpressure.
     * @note write() reports queue acceptance; sink failures are exposed separately.
     */
    template<LoggerPolicy Policy, LoggerMetadataPolicy Metadata>
    class PolicyLogger<Policy, AsyncSinkDispatch, Metadata> final : public ILogger
    {
    public:
        PolicyLogger()
            : worker_([this](std::stop_token) { run(); })
        {
        }

        template<SinkType... Sinks>
        explicit PolicyLogger(Sinks&... sinks)
            : backend_(sinks...), worker_([this](std::stop_token) { run(); })
        {
        }

        template<SinkType Sink>
        explicit PolicyLogger(std::shared_ptr<Sink> sink)
            : backend_(std::move(sink)), worker_([this](std::stop_token) { run(); })
        {
        }

        PolicyLogger(const PolicyLogger&) = delete;
        PolicyLogger& operator=(const PolicyLogger&) = delete;

        [[nodiscard]] bool attach(ILogSink& sink) override { return backend_.attach(sink); }
        [[nodiscard]] bool attach(std::shared_ptr<ILogSink> sink)
        {
            return backend_.attach(std::move(sink));
        }
        [[nodiscard]] bool detach(ILogSink& sink) override { return backend_.detach(sink); }

        [[nodiscard]] bool write(Level level, const Error& error) override
        {
            return enqueue(level, error);
        }

        [[nodiscard]] bool write_owned(Level level, Error error) override
        {
            return enqueue(level, std::move(error));
        }

        /** @brief Waits until every accepted record has reached the sink callbacks. */
        void flush()
        {
            std::unique_lock lock{mutex_};
            drained_.wait(lock, [this] { return queue_.empty() && !batch_active_; });
        }

        [[nodiscard]] std::size_t failed_records() const noexcept
        {
            return failed_records_.load(std::memory_order_relaxed);
        }

        void shutdown() noexcept
        {
            const std::lock_guard shutdown_lock{shutdown_mutex_};
            {
                const std::lock_guard lock{mutex_};
                stopping_ = true;
            }
            records_available_.notify_all();
            space_available_.notify_all();
            if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
            {
                worker_.join();
            }
        }

        ~PolicyLogger() noexcept { shutdown(); }

    private:
        struct PendingRecord
        {
            Level level;
            Error error;
        };

        template<typename ErrorValue>
        [[nodiscard]] bool enqueue(Level level, ErrorValue&& error)
        {
            if (!Policy::accepts(level))
            {
                return true;
            }

            std::unique_lock lock{mutex_};
            space_available_.wait(lock, [this]
            {
                return stopping_ || queue_.size() < AsyncSinkDispatch::queue_capacity;
            });
            if (stopping_)
            {
                return false;
            }

            queue_.push_back(PendingRecord{level, std::forward<ErrorValue>(error)});
            lock.unlock();
            records_available_.notify_one();
            return true;
        }

        void run() noexcept
        {
            std::deque<PendingRecord> batch;
            while (true)
            {
                {
                    std::unique_lock lock{mutex_};
                    records_available_.wait(lock, [this]
                    {
                        return stopping_ || !queue_.empty();
                    });
                    if (queue_.empty() && stopping_)
                    {
                        drained_.notify_all();
                        return;
                    }
                    batch.swap(queue_);
                    batch_active_ = true;
                }
                space_available_.notify_all();

                for (auto& record : batch)
                {
                    try
                    {
                        if (!backend_.write_owned(record.level, std::move(record.error)))
                        {
                            failed_records_.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    catch (...)
                    {
                        failed_records_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                batch.clear();

                {
                    const std::lock_guard lock{mutex_};
                    batch_active_ = false;
                    if (queue_.empty())
                    {
                        drained_.notify_all();
                    }
                }
            }
        }

        PolicyLogger<Policy, ParallelSinkDispatch, Metadata> backend_;
        mutable std::mutex mutex_;
        std::condition_variable records_available_;
        std::condition_variable space_available_;
        std::condition_variable drained_;
        std::deque<PendingRecord> queue_;
        std::jthread worker_;
        std::mutex shutdown_mutex_;
        std::atomic<std::size_t> failed_records_ = 0;
        bool stopping_ = false;
        bool batch_active_ = false;
    };

    /** @brief Backward-compatible logger with no level filtering. */
    using Logger = PolicyLogger<AcceptAllPolicy>;

    /**
     * @brief High-throughput logger for sinks with thread-safe write implementations.
     * @note Unlike Logger, concurrent calls may enter an attached sink simultaneously.
     */
    using ParallelLogger = PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch>;

    /**
     * @brief Thread-safe text sink writing one record per line.
     */
    class ConsoleSink final : public ILogSink
    {
    public:
        explicit ConsoleSink(std::ostream& output) noexcept
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
}
