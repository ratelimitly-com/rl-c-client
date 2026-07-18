cmake_minimum_required(VERSION 3.16)
project(rl_glib_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

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
add_executable(glib-example main.c)
target_compile_features(glib-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(glib-example PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(glib-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(glib-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(glib-example PRIVATE
    rclient PkgConfig::GIO OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(glib-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(glib-example PRIVATE resolv)
endif()
