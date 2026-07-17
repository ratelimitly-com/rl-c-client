cmake_minimum_required(VERSION 3.16)
project(rl_win32_example C)

if(NOT WIN32)
    message(FATAL_ERROR "The native Win32 example requires a Windows target")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(RL_CLIENT_LIBRARY "${RL_CLIENT_ROOT}/librclient.a" CACHE FILEPATH
    "Path to the Windows rl-c-client static library")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES IMPORTED_LOCATION "${RL_CLIENT_LIBRARY}")
add_executable(win32-example main.c)
target_compile_features(win32-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(win32-example PRIVATE /W4 /WX)
else()
    target_compile_options(win32-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(win32-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(win32-example PRIVATE
    rclient OpenSSL::Crypto ws2_32 dnsapi)
