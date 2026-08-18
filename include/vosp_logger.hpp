#pragma once

#include "vosp_error.hpp"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <string_view>
#include <thread>
#include <type_traits>
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
     * @note A sink must outlive the Logger that references it.
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

        [[nodiscard]] bool debug(const Error& error)
        {
            return write(Level::DEBUG, error);
        }

        [[nodiscard]] bool info(const Error& error)
        {
            return write(Level::INFO, error);
        }

        [[nodiscard]] bool warning(const Error& error)
        {
            return write(Level::WARNING, error);
        }

        [[nodiscard]] bool error(const Error& error_value)
        {
            return write(Level::ERROR, error_value);
        }

        [[nodiscard]] bool critical(const Error& error)
        {
            return write(Level::CRITICAL, error);
        }

        virtual ~ILogger() noexcept = default;
    };

    /**
     * @brief Thread-safe logger that broadcasts records to non-owning sinks.
     */
    template<LoggerPolicy Policy = AcceptAllPolicy>
    class PolicyLogger final : public ILogger
    {
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
         * @brief Adds a sink to the broadcast list.
         * @param sink Sink that must outlive this logger.
         * @return false when the sink is already registered.
         */
        [[nodiscard]] bool attach(ILogSink& sink) override
        {
            const std::lock_guard lock{mutex_};

            for (const auto& registered_sink : sinks_)
            {
                if (&registered_sink.get() == &sink)
                {
                    return false;
                }
            }

            sinks_.emplace_back(sink);
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
                    return &registered_sink.get() == &sink;
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
            if (!Policy::accepts(level))
            {
                return true;
            }

            const LogEntry entry{
                std::chrono::system_clock::now(),
                std::this_thread::get_id(),
                level,
                error
            };

            std::vector<std::reference_wrapper<ILogSink>> sinks;
            {
                const std::lock_guard lock{mutex_};

                if (sinks_.empty())
                {
                    return false;
                }

                sinks = sinks_;
            }

            const std::lock_guard callback_lock{callback_mutex_};
            bool accepted = true;
            for (auto& sink : sinks)
            {
                accepted = sink.get().write(entry) && accepted;
            }

            return accepted;
        }

    private:
        std::mutex mutex_;
        std::recursive_mutex callback_mutex_;
        std::vector<std::reference_wrapper<ILogSink>> sinks_;
    };

    /** @brief Backward-compatible logger with no level filtering. */
    using Logger = PolicyLogger<AcceptAllPolicy>;

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
