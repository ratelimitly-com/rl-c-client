cmake_minimum_required(VERSION 3.16)
project(rl_facil_io_example C)

if(WIN32)
    message(FATAL_ERROR "facil.io 0.7 and this example target POSIX hosts")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(FACIL_ROOT "" CACHE PATH "Path to the facil.io 0.7 source tree")
if(NOT EXISTS "${FACIL_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "Set FACIL_ROOT to a facil.io 0.7 source tree")
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
# facil.io 0.7 predates CMake 4's minimum-policy enforcement.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${FACIL_ROOT}" "${CMAKE_BINARY_DIR}/facil-build" EXCLUDE_FROM_ALL)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(facil-io-example main.c)
target_compile_features(facil-io-example PRIVATE c_std_11)
target_compile_options(facil-io-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(facil-io-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_include_directories(facil-io-example SYSTEM PRIVATE
    "${FACIL_ROOT}/lib" "${FACIL_ROOT}/lib/facil"
    "${FACIL_ROOT}/lib/facil/tls" "${FACIL_ROOT}/lib/facil/fiobj"
    "${FACIL_ROOT}/lib/facil/cli" "${FACIL_ROOT}/lib/facil/http"
    "${FACIL_ROOT}/lib/facil/http/parsers" "${FACIL_ROOT}/lib/facil/redis")
target_link_libraries(facil-io-example PRIVATE
    rclient facil.io OpenSSL::Crypto OpenSSL::SSL
    Threads::Threads resolv ${CMAKE_DL_LIBS} m)
