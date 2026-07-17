cmake_minimum_required(VERSION 3.16)
project(rl_liburing_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The liburing example is Linux-only")
endif()

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(liburing-example main.c)
target_compile_features(liburing-example PRIVATE c_std_11)
target_compile_options(liburing-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(liburing-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(liburing-example PRIVATE
    rclient PkgConfig::LIBURING OpenSSL::Crypto Threads::Threads resolv)
