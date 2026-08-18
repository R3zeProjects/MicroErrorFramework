#include "vosp.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Json = nlohmann::json;
    using namespace vosp::error;

    bool check(bool condition, const char* message)
    {
        if (condition)
        {
            return true;
        }

        std::cerr << "FAILED: " << message << '\n';
        return false;
    }

    std::string make_document(std::size_t index)
    {
        const Json document{
            {"id", index},
            {"service", "micro-error-system"},
            {"payload", {"attempt", index % 11, "valid", index % 3 != 0}}
        };

        const std::string serialized = document.dump();
        if (index % 3 == 0)
        {
            return serialized.substr(0, serialized.size() - 1);
        }

        return serialized;
    }
}

int main()
{
    constexpr std::size_t document_count = 20000;
    constexpr std::size_t expected_failures = (document_count + 2) / 3;

    vosp::async::IndustrialWorkerPool pool{4, 64};
    MemoryRegister<Category::DATABASE> register_instance{document_count};
    MultiThreadedSystem<decltype(register_instance)> system{register_instance};

    std::vector<std::future<OperationResult>> tasks;
    tasks.reserve(document_count);
    for (std::size_t index = 0; index < document_count; ++index)
    {
        tasks.push_back(pool.submit(
            [&system, document = make_document(index), index]() -> OperationResult
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

    bool all_completed = true;
    for (auto& task : tasks)
    {
        all_completed = task.get().has_value() && all_completed;
    }

    pool.shutdown(vosp::async::ShutdownMode::DRAIN);

    return check(all_completed, "all external parser tasks completed") &&
           check(register_instance.size() == expected_failures,
                 "all malformed documents were registered") &&
           check(pool.pending_tasks() == 0, "external workload queue drained")
               ? 0
               : 1;
}
