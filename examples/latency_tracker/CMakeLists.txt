cmake_minimum_required(VERSION 3.16)
project(rl_latency_tracker_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")

add_executable(latency-tracker-example main.c)
target_compile_features(latency-tracker-example PRIVATE c_std_11)
target_compile_options(latency-tracker-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(latency-tracker-example PRIVATE
    "${RL_CLIENT_ROOT}/include")
target_link_libraries(latency-tracker-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads)

if(WIN32)
    target_link_libraries(latency-tracker-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(latency-tracker-example PRIVATE resolv)
endif()
