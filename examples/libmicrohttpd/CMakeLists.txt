cmake_minimum_required(VERSION 3.16)
project(rl_libmicrohttpd_example C)

if(WIN32)
    message(FATAL_ERROR
        "This external-select example targets Linux and macOS; use Mongoose "
        "or the Win32 example for native Windows")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED IMPORTED_TARGET libmicrohttpd)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(libmicrohttpd-example main.c)
target_compile_features(libmicrohttpd-example PRIVATE c_std_11)
target_compile_options(libmicrohttpd-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(libmicrohttpd-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(libmicrohttpd-example PRIVATE
    rclient PkgConfig::MICROHTTPD OpenSSL::Crypto Threads::Threads resolv)
