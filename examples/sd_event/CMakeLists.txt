cmake_minimum_required(VERSION 3.16)
project(rl_sd_event_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The sd-event example is Linux-only")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(sd-event-example main.c)
target_compile_features(sd-event-example PRIVATE c_std_11)
target_compile_options(sd-event-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(sd-event-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(sd-event-example PRIVATE
    rclient PkgConfig::SYSTEMD OpenSSL::Crypto Threads::Threads resolv)
