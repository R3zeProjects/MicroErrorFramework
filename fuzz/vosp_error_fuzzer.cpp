#include <vosp.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace
{
    [[nodiscard]] vosp::error::Category decode_category(std::uint8_t value) noexcept
    {
        using vosp::error::Category;
        switch (value % 5U)
        {
        case 0: return Category::NETWORK;
        case 1: return Category::DATABASE;
        case 2: return Category::FILESYSTEM;
        case 3: return Category::NONE;
        default: return static_cast<Category>(0x7ffeU);
        }
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }

    const auto category = decode_category(data[0]);
    const std::uint32_t code = size > 1 ? data[1] : 0;
    const std::size_t message_size = std::min(
        size > 2 ? size - 2 : 0U,
        std::size_t{16U * 1024U});
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

    using namespace vosp::error;
    MemoryRegister<Category::NETWORK> network{16, 64};
    MemoryRegister<Category::DATABASE> database{16, 64};
    MemoryRegister<Category::FILESYSTEM> filesystem{16, 64};
    MultiThreadedSystem<decltype(network), decltype(database), decltype(filesystem)>
        system{network, database, filesystem};

    const std::string transition_message{
        message.substr(0, std::min(message.size(), std::size_t{8}))};
    const auto transition_end = std::min(size, std::size_t{66});
    for (std::size_t index = 2; index < transition_end; ++index)
    {
        const auto operation_category = decode_category(data[index]);
        const Error operation_error{
            operation_category,
            static_cast<std::uint32_t>(code + index),
            transition_message};
        switch (data[index] & 3U)
        {
        case 0:
            static_cast<void>(system.add(operation_error));
            break;
        case 1:
            static_cast<void>(system.remove(operation_error));
            break;
        case 2:
            static_cast<void>(system.add(operation_error));
            static_cast<void>(system.add(operation_error));
            break;
        default:
            static_cast<void>(network.add(operation_error));
            static_cast<void>(network.remove(operation_error));
            break;
        }
    }

    return 0;
}
