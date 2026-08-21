#include <vosp.hpp>

using InvalidLogger = vosp::logger::Logger<
    vosp::logger::logger_policy::AcceptAll,
    int>;

int main()
{
    InvalidLogger logger;
    (void)logger;
}
