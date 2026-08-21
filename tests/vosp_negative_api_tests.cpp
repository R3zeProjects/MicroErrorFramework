#include <vosp.hpp>

#include <format>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace
{
    using namespace vosp::async;
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

    bool test_formatter_rejects_specifiers()
    {
        Error error{Category::NETWORK, 1, "format"};
        try
        {
            static_cast<void>(std::vformat("{:x}", std::make_format_args(error)));
        }
        catch (const std::format_error&)
        {
            return true;
        }
        return check(false, "Error formatter rejects unsupported specifiers");
    }

    bool test_unroutable_errors_preserve_state()
    {
        Register<Category::NETWORK> network;
        System<system_policy::SingleThreaded, decltype(network)> system{network};
        const Error filesystem{Category::FILESYSTEM, 2, "unroutable"};
        const auto result = system.add(filesystem);

        return check(!result && result.error().code() == missing_register_code,
                     "system reports a missing category register") &&
               check(network.size() == 0,
                     "unroutable error does not mutate a register");
    }

    bool test_sink_rejections()
    {
        std::ostringstream failed_output;
        failed_output.setstate(std::ios::badbit);
        Sink failed_sink{failed_output};
        Logger failed_logger{failed_sink};
        const bool failed_write = failed_logger.error(
            Error{Category::FILESYSTEM, 3, "failed stream"});

        Logger logger;
        std::shared_ptr<ILogSink> null_sink;
        const bool null_attached = logger.attach(std::move(null_sink));

        bool zero_threshold_rejected = false;
        try
        {
            std::ostringstream output;
            Sink<sink_policy::Buffered> invalid{output, 0};
            static_cast<void>(invalid);
        }
        catch (const std::invalid_argument&)
        {
            zero_threshold_rejected = true;
        }

        return check(!failed_write, "failed stream rejects a log record") &&
               check(!null_attached, "null owned sink is rejected") &&
               check(zero_threshold_rejected,
                     "buffered sink rejects a zero flush threshold");
    }

    bool test_worker_rejects_invalid_queue_capacity()
    {
        try
        {
            WorkerPool pool{1, 0};
            static_cast<void>(pool);
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        return check(false, "worker pool rejects a zero queue capacity");
    }
}

int main()
{
    return test_formatter_rejects_specifiers() &&
           test_unroutable_errors_preserve_state() &&
           test_sink_rejections() &&
           test_worker_rejects_invalid_queue_capacity() ? 0 : 1;
}
