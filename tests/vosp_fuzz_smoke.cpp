#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main()
{
    std::uint32_t state = 0xC0FFEEu;
    std::array<std::uint8_t, 512> input{};

    for (std::size_t iteration = 0; iteration < 100'000; ++iteration)
    {
        state = state * 1'664'525u + 1'013'904'223u;
        const std::size_t size = state % (input.size() + 1);
        for (std::size_t index = 0; index < size; ++index)
        {
            state = state * 1'664'525u + 1'013'904'223u;
            input[index] = static_cast<std::uint8_t>(state >> 24U);
        }

        static_cast<void>(LLVMFuzzerTestOneInput(input.data(), size));
    }

    return 0;
}
