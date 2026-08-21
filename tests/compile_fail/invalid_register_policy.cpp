#include <vosp.hpp>

using InvalidRegister = vosp::error::Register<
    vosp::error::Category::NETWORK,
    int>;

int main()
{
    InvalidRegister register_instance;
    (void)register_instance;
}
