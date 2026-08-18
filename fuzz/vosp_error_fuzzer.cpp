#include <vosp.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }

    const auto category = static_cast<vosp::error::Category>(data[0] % 4);
    const std::uint32_t code = size > 1 ? data[1] : 0;
    const std::size_t message_size = std::min(size > 2 ? size - 2 : 0U, std::size_t{256});
    const std::string message{
        reinterpret_cast<const char*>(data + std::min(size, std::size_t{2})),
        message_size
    };

    const vosp::error::Error error{category, code, message};
    const vosp::error::Error copy{category, code, message};
    if (error != copy || !(error == copy))
    {
        __builtin_trap();
    }

    vosp::error::MemoryRegister<vosp::error::Category::NETWORK> register_instance;
    if (category == vosp::error::Category::NETWORK)
    {
        const auto result = register_instance.add(error);
        if (result)
        {
            static_cast<void>(register_instance.remove(error));
        }
    }

    return 0;
}
