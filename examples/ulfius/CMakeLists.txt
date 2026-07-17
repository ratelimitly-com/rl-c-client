cmake_minimum_required(VERSION 3.16)
project(rl_ulfius_example C)

if(WIN32)
    message(FATAL_ERROR "This poll-based Ulfius callback targets POSIX hosts")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ULFIUS REQUIRED IMPORTED_TARGET libulfius)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(ulfius-example main.c)
target_compile_features(ulfius-example PRIVATE c_std_11)
target_compile_options(ulfius-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(ulfius-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(ulfius-example PRIVATE
    rclient PkgConfig::ULFIUS OpenSSL::Crypto Threads::Threads resolv)
