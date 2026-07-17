cmake_minimum_required(VERSION 3.16)
project(rl_glib_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(glib-example main.c)
target_compile_features(glib-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(glib-example PRIVATE /W4 /WX)
else()
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
