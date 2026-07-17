cmake_minimum_required(VERSION 3.16)
project(rl_kqueue_example C)

if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|FreeBSD|OpenBSD|NetBSD|DragonFly")
    message(FATAL_ERROR "The kqueue example requires macOS or BSD")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(kqueue-example main.c)
target_compile_features(kqueue-example PRIVATE c_std_11)
target_compile_options(kqueue-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(kqueue-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(kqueue-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads resolv)
