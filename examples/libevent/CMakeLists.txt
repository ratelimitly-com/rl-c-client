cmake_minimum_required(VERSION 3.16)
project(rl_libevent_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBEVENT QUIET IMPORTED_TARGET libevent)
endif()
if(NOT TARGET PkgConfig::LIBEVENT)
    find_path(LIBEVENT_INCLUDE_DIR event2/event.h REQUIRED)
    find_library(LIBEVENT_LIBRARY NAMES event libevent REQUIRED)
    add_library(libevent_external UNKNOWN IMPORTED)
    set_target_properties(libevent_external PROPERTIES
        IMPORTED_LOCATION "${LIBEVENT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_INCLUDE_DIR}")
endif()

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")

add_executable(libevent-example main.c)
target_compile_features(libevent-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(libevent-example PRIVATE /W4 /WX)
else()
    target_compile_options(libevent-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(libevent-example PRIVATE "${RL_CLIENT_ROOT}/include")
if(TARGET PkgConfig::LIBEVENT)
    target_link_libraries(libevent-example PRIVATE PkgConfig::LIBEVENT)
else()
    target_link_libraries(libevent-example PRIVATE libevent_external)
endif()
target_link_libraries(libevent-example PRIVATE
    rclient OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(libevent-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(libevent-example PRIVATE resolv)
endif()
