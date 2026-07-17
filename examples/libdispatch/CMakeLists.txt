cmake_minimum_required(VERSION 3.16)
project(rl_libdispatch_example C)

if(WIN32)
    message(FATAL_ERROR "This libdispatch example targets POSIX sockets")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_path(DISPATCH_INCLUDE_DIR dispatch/dispatch.h REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(libdispatch-example main.c)
target_compile_features(libdispatch-example PRIVATE c_std_11)
target_compile_options(libdispatch-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(libdispatch-example PRIVATE
    "${RL_CLIENT_ROOT}/include" "${DISPATCH_INCLUDE_DIR}")
target_link_libraries(libdispatch-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads resolv)
if(NOT APPLE)
    find_library(DISPATCH_LIBRARY dispatch REQUIRED)
    target_link_libraries(libdispatch-example PRIVATE "${DISPATCH_LIBRARY}")
endif()
