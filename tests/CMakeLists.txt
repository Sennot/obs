cmake_minimum_required(VERSION 3.16)
project(ClickSyncTests CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(core_tests core_tests.cpp ../src/session.cpp)
target_include_directories(core_tests PRIVATE ../src)

enable_testing()
add_test(NAME core_tests COMMAND core_tests)
