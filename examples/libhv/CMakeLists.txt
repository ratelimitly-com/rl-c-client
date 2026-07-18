cmake_minimum_required(VERSION 3.16)
project(rl_libhv_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(libhv CONFIG QUIET)
if(NOT TARGET hv)
    find_path(LIBHV_INCLUDE_DIR hv/hloop.h REQUIRED)
    find_library(LIBHV_LIBRARY NAMES hv libhv REQUIRED)
    add_library(hv UNKNOWN IMPORTED)
    set_target_properties(hv PROPERTIES
        IMPORTED_LOCATION "${LIBHV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBHV_INCLUDE_DIR}")
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
add_executable(libhv-example main.c)
target_compile_features(libhv-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(libhv-example PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(libhv-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(libhv-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(libhv-example PRIVATE
    rclient hv OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(libhv-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(libhv-example PRIVATE resolv)
endif()
