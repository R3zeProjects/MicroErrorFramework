#include <vosp.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/sinks/FileSink.h>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
const std::string message(128, 'x');
const std::string rendered = "[INFO] [NETWORK] code=1001 message=" + message;

struct Measurement {
  double throughput = 0.0;
  std::uintmax_t bytes = 0;
};

void remove_file(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

template <typename Operation>
void run_producers(std::size_t operations, std::size_t producers,
                   Operation &&operation) {
  std::atomic<bool> successful = true;
  std::latch ready{static_cast<std::ptrdiff_t>(producers)};
  std::latch start{1};
  std::vector<std::jthread> workers;
  workers.reserve(producers);
  for (std::size_t producer = 0; producer < producers; ++producer) {
    workers.emplace_back([&, producer] {
      ready.count_down();
      start.wait();
      for (std::size_t index = producer; index < operations;
           index += producers) {
        if (!operation()) {
          successful.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  ready.wait();
  start.count_down();
  workers.clear();
  if (!successful.load(std::memory_order_relaxed)) {
    throw std::runtime_error{"logger rejected a benchmark record"};
  }
}

template <typename Work>
[[nodiscard]] Measurement measure(const std::filesystem::path &path,
                                  std::size_t operations, Work &&work) {
  remove_file(path);
  const auto begin = Clock::now();
  work();
  const auto elapsed =
      std::chrono::duration<double>(Clock::now() - begin).count();
  return {static_cast<double>(operations) / elapsed,
          std::filesystem::file_size(path)};
}

[[nodiscard]] Measurement run_mef(const std::filesystem::path &path,
                                  std::size_t operations, std::size_t producers,
                                  bool asynchronous) {
  return measure(path, operations, [&] {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    vsp::logger::Sink<vsp::logger::sink_policy::Buffered> sink{output};
    const vsp::error::Error error{vsp::error::Category::NETWORK, 1001,
                                  std::string{message}};
    if (asynchronous) {
      vsp::logger::PolicyLogger<vsp::logger::logger_policy::AcceptAll,
                                vsp::logger::logger_policy::Async,
                                vsp::logger::logger_policy::MinimalMetadata>
          logger{sink};
      run_producers(operations, producers, [&] { return logger.info(error); });
      logger.flush();
    } else {
      vsp::logger::PolicyLogger<vsp::logger::logger_policy::AcceptAll,
                                vsp::logger::logger_policy::Parallel,
                                vsp::logger::logger_policy::MinimalMetadata>
          logger{sink};
      run_producers(operations, producers, [&] { return logger.info(error); });
    }
    if (!sink.flush()) {
      throw std::runtime_error{"MEF sink flush failed"};
    }
  });
}

[[nodiscard]] Measurement run_spdlog(const std::filesystem::path &path,
                                     std::size_t operations,
                                     std::size_t producers, bool asynchronous) {
  if (asynchronous) {
    throw std::invalid_argument{
        "spdlog async is not part of this comparison contract"};
  }
  return measure(path, operations, [&] {
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        path.string(), true);
    spdlog::logger logger{"external-benchmark", std::move(sink)};
    logger.set_formatter(std::make_unique<spdlog::pattern_formatter>(
        "%v", spdlog::pattern_time_type::local, "\n"));
    run_producers(operations, producers, [&] {
      logger.info("{}", rendered);
      return true;
    });
    logger.flush();
  });
}

[[nodiscard]] Measurement run_quill(const std::filesystem::path &path,
                                    std::size_t operations,
                                    std::size_t producers, bool asynchronous) {
  if (!asynchronous) {
    throw std::invalid_argument{
        "Quill is measured only with its asynchronous contract"};
  }
  remove_file(path);
  try {
    const auto result = measure(path, operations, [&] {
      quill::Backend::start();
      quill::FileSinkConfig config;
      config.set_open_mode('w');
      auto sink = quill::Frontend::create_or_get_sink<quill::FileSink>(
          path.string(), config, quill::FileEventNotifier{});
      quill::PatternFormatterOptions pattern{"%(message)"};
      auto *logger = quill::Frontend::create_or_get_logger(
          "external-benchmark", std::move(sink), std::move(pattern));
      run_producers(operations, producers, [&] {
        QUILL_LOG_INFO(logger, "{}", rendered);
        return true;
      });
      logger->flush_log();
      quill::Backend::stop();
    });
    return result;
  } catch (...) {
    quill::Backend::stop();
    throw;
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 4) {
      throw std::invalid_argument{
          "usage: benchmark <mef-sync|mef-async|spdlog-sync|quill-async> "
          "[operations] [producers]"};
    }
    const std::string_view scenario{argv[1]};
    const auto operations = argc > 2 ? std::stoull(argv[2]) : 500'000ULL;
    const auto producers = argc > 3 ? std::stoull(argv[3]) : 1ULL;
    if (operations == 0 || producers == 0) {
      throw std::invalid_argument{"operations and producers must be non-zero"};
    }
    const auto path = std::filesystem::temp_directory_path() /
                      ("mef-external-" + std::string{scenario} + ".log");

    Measurement result;
    if (scenario == "mef-sync")
      result = run_mef(path, operations, producers, false);
    else if (scenario == "mef-async")
      result = run_mef(path, operations, producers, true);
    else if (scenario == "spdlog-sync")
      result = run_spdlog(path, operations, producers, false);
    else if (scenario == "quill-async")
      result = run_quill(path, operations, producers, true);
    else
      throw std::invalid_argument{"unknown benchmark scenario"};

    const auto expected_bytes = operations * (rendered.size() + 1U);
    if (result.bytes != expected_bytes) {
      throw std::runtime_error{
          "output size does not match the common record contract"};
    }
    std::cout
        << "scenario,operations,producers,throughput_per_second,file_bytes\n"
        << scenario << ',' << operations << ',' << producers << ','
        << result.throughput << ',' << result.bytes << '\n';
    remove_file(path);
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "external logger benchmark failed: " << exception.what()
              << '\n';
    return 1;
  }
}
