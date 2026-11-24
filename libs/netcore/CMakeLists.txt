cmake_minimum_required(VERSION 3.25)
project(black_netcore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(netcore STATIC
    src/protocol.cpp
    src/rules.cpp
    src/session.cpp
    src/territorial.cpp
)

target_include_directories(netcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

option(NETCORE_BUILD_TESTS "Build netcore unit tests" ON)
if(NETCORE_BUILD_TESTS)
    enable_testing()
    add_executable(netcore_tests
        tests/test_netcore.cpp
    )
    target_link_libraries(netcore_tests PRIVATE netcore)
    add_test(NAME netcore_tests COMMAND netcore_tests)
    add_executable(territorial_tests tests/test_territorial.cpp)
    target_link_libraries(territorial_tests PRIVATE netcore)
    add_test(NAME territorial_tests COMMAND territorial_tests)
endif()
