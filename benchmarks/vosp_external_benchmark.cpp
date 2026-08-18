#include <vosp.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using Json = nlohmann::json;
    using namespace vosp::error;

    std::string make_document(std::size_t index)
    {
        const Json document{
            {"id", index},
            {"service", "micro-error-system"},
            {"payload", {"attempt", index % 11, "valid", index % 3 != 0}}
        };

        const std::string serialized = document.dump();
        return index % 3 == 0
                   ? serialized.substr(0, serialized.size() - 1)
                   : serialized;
    }

    double operations_per_second(std::size_t operations, std::int64_t elapsed_us)
    {
        return elapsed_us == 0
                   ? 0.0
                   : static_cast<double>(operations) * 1'000'000.0 /
                         static_cast<double>(elapsed_us);
    }
}

int main()
{
    constexpr std::size_t document_count = 20'000;
    constexpr std::size_t expected_failures = (document_count + 2) / 3;
    std::vector<std::string> documents;
    documents.reserve(document_count);
    for (std::size_t index = 0; index < document_count; ++index)
    {
        documents.push_back(make_document(index));
    }

    std::size_t parse_failures = 0;
    const auto parse_start = Clock::now();
    for (const auto& document : documents)
    {
        try
        {
            static_cast<void>(Json::parse(document));
        }
        catch (const Json::parse_error&)
        {
            ++parse_failures;
        }
    }
    const auto parse_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - parse_start).count();

    vosp::async::IndustrialWorkerPool pool{4, 64};
    MemoryRegister<Category::DATABASE> register_instance{document_count};
    MultiThreadedSystem<decltype(register_instance)> system{register_instance};
    std::vector<std::future<OperationResult>> tasks;
    tasks.reserve(document_count);

    const auto control_start = Clock::now();
    for (std::size_t index = 0; index < document_count; ++index)
    {
        tasks.push_back(pool.submit(
            [&system, document = documents[index], index]() -> OperationResult
            {
                try
                {
                    static_cast<void>(Json::parse(document));
                    return {};
                }
                catch (const Json::parse_error& exception)
                {
                    return system.add(Error{
                        Category::DATABASE,
                        static_cast<std::uint32_t>(index),
                        exception.what()
                    });
                }
            }));
    }

    bool completed = true;
    for (auto& task : tasks)
    {
        completed = task.get().has_value() && completed;
    }
    pool.shutdown(vosp::async::ShutdownMode::DRAIN);
    const auto control_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - control_start).count();

    if (!completed || parse_failures != expected_failures ||
        register_instance.size() != expected_failures)
    {
        std::cerr << "external benchmark validation failed\n";
        return 1;
    }

    std::cout << "external_repo=nlohmann/json@v3.12.0"
              << " documents=" << document_count
              << " malformed=" << expected_failures << '\n'
              << "parse_only elapsed_us=" << parse_elapsed
              << " operations_per_second="
              << operations_per_second(document_count, parse_elapsed) << '\n'
              << "parse_and_error_control workers=" << pool.worker_count()
              << " elapsed_us=" << control_elapsed
              << " operations_per_second="
              << operations_per_second(document_count, control_elapsed) << '\n';
}
