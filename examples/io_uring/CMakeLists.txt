cmake_minimum_required(VERSION 3.16)
project(rl_raw_io_uring_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The raw io_uring example is Linux-only")
endif()

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(io-uring-example main.c)
target_compile_features(io-uring-example PRIVATE c_std_11)
target_compile_options(io-uring-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(io-uring-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(io-uring-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads resolv)
