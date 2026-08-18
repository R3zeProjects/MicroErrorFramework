#pragma once

#include "vosp_error.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
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

    /** @brief Restricts a type to a compile-time sink dispatch policy. */
    template<typename Dispatch>
    concept LoggerDispatchPolicy = requires
    {
        typename std::bool_constant<Dispatch::serializes_callbacks>;
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
     * @brief Non-owning log record for the direct high-throughput logging path.
     * @warning message is valid only for the duration of the sink write callback.
     */
    struct FastLogEntry
    {
        Level level;
        Category category;
        std::uint32_t code;
        std::string_view message;
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

    /** @brief Restricts a type to a direct sink for FastLogger. */
    template<typename Sink>
    concept FastSink = requires(Sink& sink, const FastLogEntry& entry)
    {
        { sink.write(entry) } -> std::same_as<bool>;
    };

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
             LoggerDispatchPolicy Dispatch = SerializedSinkDispatch>
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
                std::chrono::system_clock::now(),
                std::this_thread::get_id(),
                level,
                std::forward<ErrorValue>(error)
            };

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

        std::mutex mutex_;
        std::recursive_mutex callback_mutex_;
        std::vector<SinkSlot> sinks_;
    };

    /** @brief Backward-compatible logger with no level filtering. */
    using Logger = PolicyLogger<AcceptAllPolicy>;

    /**
     * @brief High-throughput logger for sinks with thread-safe write implementations.
     * @note Unlike Logger, concurrent calls may enter an attached sink simultaneously.
     */
    using ParallelLogger = PolicyLogger<AcceptAllPolicy, ParallelSinkDispatch>;

    /**
     * @brief Direct logger optimized for a fixed, thread-safe sink type.
     * @tparam Sink Concrete sink type accepting FastLogEntry.
     * @tparam Policy Compile-time log-level filter.
     * @warning The sink must outlive this logger and be thread-safe when used concurrently.
     * @note This synchronous path does not capture a timestamp or thread id and never owns
     *       the message. Use Logger or ParallelLogger when dynamic sinks or full metadata
     *       are required.
     */
    template<FastSink Sink, LoggerPolicy Policy = AcceptAllPolicy>
    class FastLogger final
    {
    public:
        explicit FastLogger(Sink& sink) noexcept
            : sink_(sink)
        {
        }

        /**
         * @brief Publishes a borrowed record without allocating an Error.
         * @warning The sink must not retain message after this call returns.
         */
        [[nodiscard]] bool log(
            Level level,
            Category category,
            std::uint32_t code,
            std::string_view message)
        {
            if (!Policy::accepts(level))
            {
                return true;
            }

            return sink_.write(FastLogEntry{level, category, code, message});
        }

    private:
        Sink& sink_;
    };

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
