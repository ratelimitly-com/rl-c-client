cmake_minimum_required(VERSION 3.16)
project(rl_libreactor_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "libreactor v3 and this example target Linux")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(REACTOR REQUIRED IMPORTED_TARGET libreactor)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(libreactor-example main.c)
target_compile_features(libreactor-example PRIVATE c_std_11)
target_compile_options(libreactor-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(libreactor-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(libreactor-example PRIVATE
    rclient PkgConfig::REACTOR OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads resolv)
