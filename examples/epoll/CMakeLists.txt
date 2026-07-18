cmake_minimum_required(VERSION 3.16)
project(rl_epoll_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The epoll example is Linux-only")
endif()

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(epoll-example main.c)
target_compile_features(epoll-example PRIVATE c_std_11)
target_compile_options(epoll-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(epoll-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(epoll-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads resolv)
