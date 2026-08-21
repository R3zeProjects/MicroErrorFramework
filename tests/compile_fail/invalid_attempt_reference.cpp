#include <vosp.hpp>

int main()
{
    int value = 42;
    const vosp::error::Error context{
        vosp::error::Category::DATABASE,
        1,
        "reference result is not owned"
    };

    // Result<T> owns T, so an exception boundary cannot return Result<int&>.
    const auto result = vosp::error::attempt(context, [&]() -> int& { return value; });
    return result ? 0 : 1;
}
