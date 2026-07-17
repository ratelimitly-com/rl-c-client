cmake_minimum_required(VERSION 3.16)
project(rl_civetweb_example C)

if(WIN32)
    message(FATAL_ERROR
        "This pthread/pipe bridge targets Linux and macOS; use the Mongoose "
        "or Win32 example for a native Windows integration")
endif()

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(CIVETWEB_ROOT "" CACHE PATH "Path to the CivetWeb source tree")
if(NOT EXISTS "${CIVETWEB_ROOT}/src/civetweb.c")
    message(FATAL_ERROR "Set CIVETWEB_ROOT to a CivetWeb source tree")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_library(civetweb_vendor STATIC "${CIVETWEB_ROOT}/src/civetweb.c")
target_include_directories(civetweb_vendor PUBLIC "${CIVETWEB_ROOT}/include")
target_compile_definitions(civetweb_vendor PRIVATE NO_SSL _GNU_SOURCE)

add_executable(civetweb-example main.c)
target_compile_features(civetweb-example PRIVATE c_std_11)
target_compile_options(civetweb-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(civetweb-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(civetweb-example PRIVATE
    rclient civetweb_vendor OpenSSL::Crypto Threads::Threads resolv
    ${CMAKE_DL_LIBS})
