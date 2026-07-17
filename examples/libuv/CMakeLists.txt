cmake_minimum_required(VERSION 3.16)
project(rl_libuv_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
endif()

if(NOT TARGET PkgConfig::LIBUV)
    find_path(LIBUV_INCLUDE_DIR uv.h REQUIRED)
    find_library(LIBUV_LIBRARY NAMES uv libuv REQUIRED)
    add_library(libuv_external UNKNOWN IMPORTED)
    set_target_properties(libuv_external PROPERTIES
        IMPORTED_LOCATION "${LIBUV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUV_INCLUDE_DIR}")
endif()

set(RL_CLIENT_SOURCES
    "${RL_CLIENT_ROOT}/src/r_client.c"
    "${RL_CLIENT_ROOT}/src/r_client_runtime.c"
    "${RL_CLIENT_ROOT}/src/r_client_workflow.c"
    "${RL_CLIENT_ROOT}/src/r_protocol.c"
    "${RL_CLIENT_ROOT}/src/r_crypto.c"
    "${RL_CLIENT_ROOT}/src/r_policy.c")
add_library(rclient STATIC ${RL_CLIENT_SOURCES})
target_compile_features(rclient PUBLIC c_std_11)
target_include_directories(rclient
    PUBLIC "${RL_CLIENT_ROOT}/include"
    PRIVATE "${RL_CLIENT_ROOT}/src")
target_link_libraries(rclient PUBLIC OpenSSL::Crypto)

add_executable(libuv-example main.c)
target_compile_features(libuv-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(libuv-example PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(libuv-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(libuv-example PRIVATE "${RL_CLIENT_ROOT}/include")

if(TARGET PkgConfig::LIBUV)
    target_link_libraries(libuv-example PRIVATE PkgConfig::LIBUV)
else()
    target_link_libraries(libuv-example PRIVATE libuv_external)
endif()
target_link_libraries(libuv-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(libuv-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(libuv-example PRIVATE resolv)
endif()
