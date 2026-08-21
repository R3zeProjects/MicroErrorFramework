cmake_minimum_required(VERSION 3.25)
project(VospPackageConsumer LANGUAGES CXX)

find_package(vosp 0.2.2 CONFIG REQUIRED)

add_executable(vosp_package_consumer main.cpp)
target_link_libraries(vosp_package_consumer PRIVATE vosp::vosp)
