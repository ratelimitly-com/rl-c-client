cmake_minimum_required(VERSION 3.16)
project(rl_onion_example C)

if(WIN32)
    message(FATAL_ERROR "This Onion long-poll bridge targets POSIX hosts")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(ONION_ROOT "" CACHE PATH "Onion v0.8 installation prefix")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_path(ONION_INCLUDE_DIR onion/onion.h HINTS "${ONION_ROOT}/include" REQUIRED)
find_library(ONION_LIBRARY onion HINTS "${ONION_ROOT}/lib" REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(onion-example main.c)
target_compile_features(onion-example PRIVATE c_std_11)
target_compile_options(onion-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(onion-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_include_directories(onion-example SYSTEM PRIVATE "${ONION_INCLUDE_DIR}")
target_link_libraries(onion-example PRIVATE
    rclient "${ONION_LIBRARY}" OpenSSL::Crypto Threads::Threads resolv)
