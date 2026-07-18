cmake_minimum_required(VERSION 3.16)
project(rl_libev_example C)

if(WIN32)
    message(FATAL_ERROR "This libev example targets Unix fd backends")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_path(LIBEV_INCLUDE_DIR ev.h REQUIRED)
find_library(LIBEV_LIBRARY NAMES ev libev REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_library(libev_external UNKNOWN IMPORTED)
set_target_properties(libev_external PROPERTIES
    IMPORTED_LOCATION "${LIBEV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBEV_INCLUDE_DIR}")
add_executable(libev-example main.c)
target_compile_features(libev-example PRIVATE c_std_11)
target_compile_options(libev-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(libev-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(libev-example PRIVATE
    rclient libev_external OpenSSL::Crypto Threads::Threads resolv)
