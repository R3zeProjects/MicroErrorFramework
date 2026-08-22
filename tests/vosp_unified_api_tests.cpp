#include <vosp.hpp>

#include <concepts>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using namespace vosp::async;
using namespace vosp::error;
using namespace vosp::logger;

static_assert(std::same_as<vsp::Logger<>, vosp::logger::Logger<>>);

bool check(bool condition, const char *message) {
  if (condition) {
    return true;
  }

  std::cerr << "FAILED: " << message << '\n';
  return false;
}

bool test_register_and_system_policies() {
  Register<Category::NETWORK, register_policy::SingleThreaded> network;
  Register<Category::DATABASE, register_policy::SingleThreaded> database;
  System<system_policy::MultiThreaded, decltype(network)> system{network};

  const Error network_error{Category::NETWORK, 1, "network"};
  if (!check(system.add(network_error).has_value(),
             "unified synchronized system adds an error") ||
      !check(network.contains(network_error),
             "unified register stores the routed error")) {
    return false;
  }

  std::vector<std::jthread> producers;
  for (std::uint32_t producer = 0; producer < 4; ++producer) {
    producers.emplace_back([&system, producer] {
      for (std::uint32_t index = 0; index < 100; ++index) {
        static_cast<void>(system.add(Error{
            Category::NETWORK, 100 + producer * 100 + index, "concurrent"}));
      }
    });
  }
  producers.clear();
  if (!check(network.size() == 401,
             "system policy serializes a register without internal locking")) {
    return false;
  }

  System<system_policy::SingleThreaded, decltype(database)> direct{database};
  const Error database_error{Category::DATABASE, 2, "database"};
  return check(direct.add(database_error).has_value(),
               "single-threaded policy adds an error") &&
         check(database.contains(database_error),
               "single-threaded register stores the error");
}

bool test_sink_and_logger_policies() {
  std::ostringstream immediate_output;
  Sink immediate{immediate_output};
  vsp::Logger logger{immediate};
  const Error immediate_error{Category::NETWORK, 3, "immediate"};
  if (!check(logger.info(immediate_error), "default logger accepts an error") ||
      !check(immediate_output.str().contains("message=immediate"),
             "immediate sink writes the record")) {
    return false;
  }

  std::ostringstream buffered_output;
  Sink buffered{buffered_output, 4096};
  Logger<logger_policy::AcceptAll, logger_policy::Parallel,
         logger_policy::MinimalMetadata>
      parallel{buffered};
  const Error buffered_error{Category::FILESYSTEM, 4, "buffered"};
  return check(parallel.warning(buffered_error),
               "policy logger accepts a buffered record") &&
         check(buffered_output.str().empty(), "buffered sink delays output") &&
         check(buffered.flush(), "buffered sink flush succeeds") &&
         check(buffered_output.str().contains("message=buffered"),
               "buffered sink writes the record on flush");
}

bool test_async_system_policy() {
  WorkerPool workers{2, 8};
  Register<Category::FILESYSTEM> filesystem;
  System<system_policy::Async<WorkerPool>, decltype(filesystem)> system{
      workers, filesystem};
  const Error error{Category::FILESYSTEM, 5, "async"};
  auto result = system.add(error);
  const bool succeeded = result.get().has_value();
  workers.shutdown(ShutdownMode::DRAIN);

  return check(succeeded, "async system policy completes through WorkerPool") &&
         check(filesystem.contains(error),
               "async system stores the routed error");
}
} // namespace

int main() {
  return test_register_and_system_policies() &&
                 test_sink_and_logger_policies() && test_async_system_policy()
             ? 0
             : 1;
}
