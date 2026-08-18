#include <vosp.hpp>

int main()
{
    vosp::error::MemoryRegister<vosp::error::Category::NETWORK> errors;
    const auto result = errors.add(
        vosp::error::Error{vosp::error::Category::NETWORK, 1, "package consumer"});
    return result ? 0 : 1;
}
