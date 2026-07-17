cmake_minimum_required(VERSION 3.16)
project(rl_lwan_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Lwan and this coroutine bridge target Linux")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(LWAN_ROOT "" CACHE PATH "Path to the Lwan source tree")
set(LWAN_BUILD "${LWAN_ROOT}/build" CACHE PATH "Path to the Lwan build tree")
set(LWAN_LIBRARY "${LWAN_BUILD}/src/lib/liblwan.a")
if(NOT EXISTS "${LWAN_LIBRARY}")
    message(FATAL_ERROR "Build Lwan first and set LWAN_ROOT and LWAN_BUILD")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(lwan-example main.c)
target_compile_features(lwan-example PRIVATE c_std_11)
target_compile_options(lwan-example PRIVATE
    -Wall -Wextra -Werror "-include${LWAN_BUILD}/lwan-build-config.h")
target_include_directories(lwan-example PRIVATE
    "${RL_CLIENT_ROOT}/include" "${LWAN_ROOT}/src/lib")
target_link_libraries(lwan-example PRIVATE
    rclient "-Wl,--whole-archive" "${LWAN_LIBRARY}" "-Wl,--no-whole-archive"
    OpenSSL::Crypto Threads::Threads resolv z zstd dl m)
