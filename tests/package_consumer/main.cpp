#include <vosp.hpp>

#include <concepts>
#include <format>

int main() {
  static_assert(vsp::version::major == 0);
  static_assert(vsp::version::minor == 6);
  static_assert(vsp::version::patch == 0);
  static_assert(std::same_as<vsp::Logger<>, vosp::logger::Logger<>>);

  vsp::error::MemoryRegister<vsp::error::Category::NETWORK> errors;
  const vsp::error::Error error{vsp::error::Category::NETWORK, 1,
                                "package consumer"};
  const auto result = errors.add(error);
  const auto stored = errors.find(1);
  const auto captured = vsp::error::attempt(error, [] { return 42; });
  return result && stored && *stored == error && captured && *captured == 42 &&
                 vsp::error::to_string(error) ==
                     "[NETWORK:1] package consumer" &&
                 std::format("{}", error) == vsp::error::to_string(error)
             ? 0
             : 1;
}
