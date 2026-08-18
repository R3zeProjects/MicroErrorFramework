#include <vosp.hpp>

#include <iostream>

int main()
{
    vosp::logger::ConsoleSink sink{std::cout};
    vosp::logger::Logger logger{sink};
    return logger.info(vosp::error::Error{
        vosp::error::Category::NETWORK,
        1001,
        "MicroErrorSystem is ready"}) ? 0 : 1;
}
