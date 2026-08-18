#include "vosp.hpp"

#include <cstdlib>
#include <fmt/format.h>
#include <httplib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

int main()
{
    using namespace vosp::error;

    constexpr std::size_t worker_count = 4;
    constexpr std::size_t requests_per_worker = 250;
    constexpr std::size_t total_requests = worker_count * requests_per_worker;
    constexpr std::size_t expected_failures = total_requests / 2;

    httplib::Server server;
    server.Get("/health", [](const httplib::Request&, httplib::Response& response)
    {
        response.set_content("ok", "text/plain");
    });
    server.Get("/error", [](const httplib::Request&, httplib::Response& response)
    {
        response.status = 503;
        response.set_content("temporarily unavailable", "text/plain");
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0)
    {
        std::cerr << "failed to bind cpp-httplib test server\n";
        return 1;
    }

    std::jthread server_thread{[&server]
    {
        static_cast<void>(server.listen_after_bind());
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{25});

    MemoryRegister<Category::NETWORK> register_instance{total_requests};
    MultiThreadedSystem<decltype(register_instance)> system{register_instance};
    std::atomic<std::size_t> successful_requests = 0;
    std::atomic<std::size_t> recorded_failures = 0;
    std::array<std::jthread, worker_count> workers;

    for (std::size_t worker = 0; worker < worker_count; ++worker)
    {
        workers[worker] = std::jthread{[&, worker]
        {
            httplib::Client client{"127.0.0.1", port};
            client.set_connection_timeout(2, 0);
            client.set_read_timeout(2, 0);

            for (std::size_t request = 0; request < requests_per_worker; ++request)
            {
                const bool expected_failure = request % 2U == 1U;
                const auto response = client.Get(expected_failure ? "/error" : "/health");
                if (response && response->status == 200 && !expected_failure)
                {
                    ++successful_requests;
                    continue;
                }

                const Error error{
                    Category::NETWORK,
                    static_cast<std::uint32_t>(5000U + worker * requests_per_worker + request),
                    fmt::format("HTTP request failed: worker={}, request={}", worker, request)
                };
                if (system.add(error))
                {
                    ++recorded_failures;
                }
            }
        }};
    }

    for (auto& worker : workers)
    {
        worker.join();
    }

    server.stop();
    server_thread.join();

    const bool passed = successful_requests == total_requests - expected_failures &&
                        recorded_failures == expected_failures &&
                        register_instance.size() == expected_failures;

    std::cout << "external_local_integration repository=fmt@11.2.0+cpp-httplib@v0.18.0"
              << " requests=" << total_requests
              << " successful=" << successful_requests
              << " recorded_failures=" << recorded_failures << '\n';

    return passed ? 0 : 1;
}
