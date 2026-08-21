#include <vosp.hpp>

#include <format>

int main()
{
    static_assert(vosp::version::major == 0);
    static_assert(vosp::version::minor == 4);
    static_assert(vosp::version::patch == 0);

    vosp::error::MemoryRegister<vosp::error::Category::NETWORK> errors;
    const vosp::error::Error error{
        vosp::error::Category::NETWORK,
        1,
        "package consumer"
    };
    const auto result = errors.add(error);
    const auto stored = errors.find(1);
    const auto captured = vosp::error::attempt(error, [] { return 42; });
    return result && stored && *stored == error && captured && *captured == 42 &&
           vosp::error::to_string(error) == "[NETWORK:1] package consumer" &&
           std::format("{}", error) == vosp::error::to_string(error) ? 0 : 1;
}
