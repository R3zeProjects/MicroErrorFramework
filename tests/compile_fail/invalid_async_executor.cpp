#include <vosp.hpp>

struct InvalidExecutor
{
};

using InvalidPolicy = vosp::error::system_policy::Async<InvalidExecutor>;
using NetworkRegister =
    vosp::error::Register<vosp::error::Category::NETWORK>;
using InvalidSystem = vosp::error::System<InvalidPolicy, NetworkRegister>;

int main()
{
    InvalidExecutor executor;
    NetworkRegister network;
    InvalidSystem system{executor, network};
    (void)system;
}
