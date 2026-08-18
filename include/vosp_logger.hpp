#pragma once

#include "vosp_error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
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
    struct [[maybe_unused]] SerializedSinkDispatch
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
    struct [[maybe_unused]] FullMetadataPolicy
    {
        [[nodiscard]] static std::chrono::system_clock::time_point timestamp() noexcept
        {
            return std::chrono::system_clock::now();
        }

        [[nodiscard]] static std::thread::id thread_id() noexcept
        {
            // A native thread keeps one id for its lifetime; caching avoids a
            // platform query on every full-metadata record.
            thread_local const auto id = std::this_thread::get_id();
            return id;
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
        std::chrono::system_clock::time_point timestamp{};
        std::thread::id thread_id{};
        Level level = Level::INFO;
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

    template<typename Value>
    concept ErrorArgument = std::same_as<std::remove_cvref_t<Value>, Error>;

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

        /** @brief Publishes a TRACE record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool trace(ErrorValue&& error)
        {
            return publish(Level::TRACE, std::forward<ErrorValue>(error));
        }

        /** @brief Publishes a DEBUG record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool debug(ErrorValue&& error)
        {
            return publish(Level::DEBUG, std::forward<ErrorValue>(error));
        }

        /** @brief Publishes an INFO record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool info(ErrorValue&& error)
        {
            return publish(Level::INFO, std::forward<ErrorValue>(error));
        }

        /** @brief Publishes a WARNING record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool warning(ErrorValue&& error)
        {
            return publish(Level::WARNING, std::forward<ErrorValue>(error));
        }

        /** @brief Publishes an ERROR record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool error(ErrorValue&& error_value)
        {
            return publish(Level::ERROR, std::forward<ErrorValue>(error_value));
        }

        /** @brief Publishes a CRITICAL record. */
        template<ErrorArgument ErrorValue>
        [[nodiscard]] bool critical(ErrorValue&& error)
        {
            return publish(Level::CRITICAL, std::forward<ErrorValue>(error));
        }

        /**
         * @brief Publishes an owned error without forcing a second message copy.
         * @note The default keeps compatibility for custom ILogger types.
         */
        [[nodiscard]] virtual bool write_owned(Level level, Error&& error)
        {
            return write(level, error);
        }

        virtual ~ILogger() noexcept = default;

    private:
        template<typename ErrorValue>
        [[nodiscard]] bool publish(Level level, ErrorValue&& error)
        {
            if constexpr (std::is_lvalue_reference_v<ErrorValue>)
            {
                return write(level, error);
            }
            else
            {
                return write_owned(level, std::forward<ErrorValue>(error));
            }
        }

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
            ILogSink* sink = nullptr;
            std::shared_ptr<ILogSink> owner{};
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
            return attach_slot(SinkSlot{&sink, {}});
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

            auto* const raw_sink = sink.get();
            return attach_slot(SinkSlot{raw_sink, std::move(sink)});
        }

        /**
         * @brief Removes a sink from the broadcast list.
         * @param sink Sink to remove.
         * @return true when the sink was registered.
         */
        [[nodiscard]] bool detach(ILogSink& sink) override
        {
            const std::lock_guard lock{mutex_};

            if (std::erase_if(sinks_, [&sink](const SinkSlot& slot)
                {
                    return slot.sink == &sink;
                }) == 0)
            {
                return false;
            }

            refresh_single_sink_fast_path();
            return true;
        }

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

        [[nodiscard]] bool write_owned(Level level, Error&& error) override
        {
            return write_entry(level, std::move(error));
        }

    private:
        [[nodiscard]] bool attach_slot(SinkSlot slot)
        {
            const std::lock_guard lock{mutex_};
            if (std::ranges::any_of(sinks_, [&slot](const SinkSlot& registered)
                {
                    return registered.sink == slot.sink;
                }))
            {
                return false;
            }

            sinks_.push_back(std::move(slot));
            refresh_single_sink_fast_path();
            return true;
        }

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

        [[nodiscard]] bool write_owned(Level level, Error&& error) override
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

        ~PolicyLogger() noexcept override { shutdown(); }

    private:
        struct PendingRecord
        {
            Level level = Level::INFO;
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

    namespace detail
    {
        struct PreparedTextRecord
        {
            std::array<char, 512> prefix{};
            std::size_t prefix_size = 0;
            std::string_view message{};
            bool message_is_buffered = false;
        };

        [[nodiscard]] inline PreparedTextRecord prepare_text_record(const LogEntry& entry)
        {
            PreparedTextRecord record;
            char* cursor = record.prefix.data();
            const auto append = [&cursor](std::string_view text)
            {
                cursor = std::copy(text.begin(), text.end(), cursor);
            };

            append("[");
            append(to_string(entry.level));
            append("] [");
            append(to_string(entry.error.category()));
            append("] code=");

            const auto code_result = std::to_chars(
                cursor,
                record.prefix.data() + record.prefix.size(),
                entry.error.code());
            cursor = code_result.ptr;
            append(" message=");

            record.message = entry.error.message();
            const auto remaining = static_cast<std::size_t>(
                record.prefix.data() + record.prefix.size() - cursor);
            if (record.message.size() < remaining)
            {
                cursor = std::copy(record.message.begin(), record.message.end(), cursor);
                *cursor++ = '\n';
                record.message_is_buffered = true;
            }
            record.prefix_size = static_cast<std::size_t>(cursor - record.prefix.data());
            return record;
        }
    }

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
            // Formatting is independent for each call. The mutex protects only
            // the shared stream and therefore never serializes stack formatting.
            const auto record = detail::prepare_text_record(entry);
            const std::lock_guard lock{mutex_};
            output_.write(
                record.prefix.data(),
                static_cast<std::streamsize>(record.prefix_size));
            if (!record.message_is_buffered)
            {
                write_view(record.message);
                output_.put('\n');
            }

            return static_cast<bool>(output_);
        }

    private:
        void write_view(std::string_view text)
        {
            constexpr auto maximum_chunk = static_cast<std::size_t>(
                std::numeric_limits<std::streamsize>::max());
            while (!text.empty() && output_)
            {
                const auto chunk_size = std::min(text.size(), maximum_chunk);
                output_.write(text.data(), static_cast<std::streamsize>(chunk_size));
                text.remove_prefix(chunk_size);
            }
        }

        std::ostream& output_;
        std::mutex mutex_;
    };

    /**
     * @brief High-throughput text sink with one internal buffer per writing thread.
     *
     * Records are accumulated without a global lock. A thread acquires the output
     * lock only after its buffer reaches the configured threshold or when flush()
     * is called. The sink preserves order within each producer thread; ordering
     * between different threads is intentionally unspecified.
     *
     * @note The stream and sink must outlive every logger operation using them.
     * @note A successful buffered write means that the record was accepted. Call
     * flush() to observe the final stream state and make buffered records visible.
     */
    class BufferedStreamSink final : public ILogSink
    {
    public:
        /** @brief Default per-thread threshold before automatic stream output. */
        static constexpr std::size_t default_flush_threshold = 64U * 1024U;

        /**
         * @param output Destination stream, which is not owned by this sink.
         * @param flush_threshold Per-thread buffered bytes before automatic output.
         * @throws std::invalid_argument if flush_threshold is zero.
         */
        explicit BufferedStreamSink(
            std::ostream& output,
            std::size_t flush_threshold = default_flush_threshold)
            : output_(output),
              flush_threshold_(flush_threshold),
              instance_id_(next_instance_id_.fetch_add(1, std::memory_order_relaxed))
        {
            if (flush_threshold_ == 0)
            {
                throw std::invalid_argument{"flush threshold must be greater than zero"};
            }
        }

        BufferedStreamSink(const BufferedStreamSink&) = delete;
        BufferedStreamSink& operator=(const BufferedStreamSink&) = delete;
        BufferedStreamSink(BufferedStreamSink&&) = delete;
        BufferedStreamSink& operator=(BufferedStreamSink&&) = delete;

        /** @brief Flushes remaining records without throwing during destruction. */
        ~BufferedStreamSink() noexcept override
        {
            try
            {
                static_cast<void>(flush());
            }
            catch (...)
            {
                // Destructors cannot report stream exceptions. Applications that
                // need delivery status should call flush() explicitly.
            }
        }

        /** @brief Accepts a record and automatically outputs a full thread buffer. */
        [[nodiscard]] bool write(const LogEntry& entry) override
        {
            const auto record = detail::prepare_text_record(entry);
            auto& shard = current_thread_shard();
            std::lock_guard shard_lock{shard.mutex};
            shard.buffer.append(record.prefix.data(), record.prefix_size);
            if (!record.message_is_buffered)
            {
                shard.buffer.append(record.message);
                shard.buffer.push_back('\n');
            }

            if (shard.buffer.size() < flush_threshold_)
            {
                return true;
            }
            return flush_shard(shard);
        }

        /**
         * @brief Writes all current thread buffers and flushes the destination stream.
         * @return true when the destination stream remains healthy.
         */
        [[nodiscard]] bool flush()
        {
            std::vector<Shard*> shards;
            {
                const std::lock_guard registry_lock{registry_mutex_};
                shards.reserve(shards_.size());
                for (auto& shard : shards_)
                {
                    shards.push_back(std::addressof(shard));
                }
            }

            bool success = true;
            for (const auto& shard : shards)
            {
                const std::lock_guard shard_lock{shard->mutex};
                success = flush_shard(*shard) && success;
            }

            const std::lock_guard output_lock{output_mutex_};
            output_.flush();
            return static_cast<bool>(output_) && success;
        }

        /** @brief Returns the configured per-thread automatic flush threshold. */
        [[nodiscard]] constexpr std::size_t flush_threshold() const noexcept
        {
            return flush_threshold_;
        }

    private:
        struct alignas(64) Shard
        {
            Shard(std::thread::id producer, std::size_t capacity)
                : producer(producer)
            {
                buffer.reserve(capacity);
            }

            std::thread::id producer;
            std::mutex mutex;
            std::string buffer;
        };

        [[nodiscard]] Shard& current_thread_shard()
        {
            thread_local std::uint64_t cached_instance_id = 0;
            thread_local Shard* cached_shard = nullptr;
            if (cached_instance_id == instance_id_)
            {
                return *cached_shard;
            }

            const auto producer = std::this_thread::get_id();
            {
                const std::lock_guard registry_lock{registry_mutex_};
                const auto existing = std::find_if(
                    shards_.begin(),
                    shards_.end(),
                    [producer](const Shard& candidate)
                    {
                        return candidate.producer == producer;
                    });
                if (existing != shards_.end())
                {
                    cached_shard = std::addressof(*existing);
                }
                else
                {
                    cached_shard = std::addressof(
                        shards_.emplace_back(producer, flush_threshold_));
                }
            }
            cached_instance_id = instance_id_;
            return *cached_shard;
        }

        [[nodiscard]] bool flush_shard(Shard& shard)
        {
            if (shard.buffer.empty())
            {
                return true;
            }

            const std::lock_guard output_lock{output_mutex_};
            output_.write(
                shard.buffer.data(),
                static_cast<std::streamsize>(shard.buffer.size()));
            shard.buffer.clear();
            return static_cast<bool>(output_);
        }

        std::ostream& output_;
        const std::size_t flush_threshold_;
        const std::uint64_t instance_id_;
        std::mutex output_mutex_;
        std::mutex registry_mutex_;
        std::deque<Shard> shards_;
        inline static std::atomic<std::uint64_t> next_instance_id_ = 1;
    };
}
