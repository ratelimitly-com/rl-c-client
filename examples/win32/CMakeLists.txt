cmake_minimum_required(VERSION 3.16)
project(rl_win32_example C)

if(NOT WIN32)
    message(FATAL_ERROR "The native Win32 example requires a Windows target")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

set(RL_CLIENT_SOURCES
    "${RL_CLIENT_ROOT}/src/r_client.c"
    "${RL_CLIENT_ROOT}/src/r_client_runtime.c"
    "${RL_CLIENT_ROOT}/src/r_client_workflow.c"
    "${RL_CLIENT_ROOT}/src/r_protocol.c"
    "${RL_CLIENT_ROOT}/src/r_crypto.c"
    "${RL_CLIENT_ROOT}/src/r_policy.c"
)

# Compile the client with the selected Windows compiler. A library produced by
# MinGW uses a different object format and C runtime from an MSVC .lib, so an
# imported librclient.a cannot make a native Visual Studio build portable.
add_library(rclient STATIC ${RL_CLIENT_SOURCES})
target_compile_features(rclient PUBLIC c_std_11)
target_include_directories(rclient
    PUBLIC "${RL_CLIENT_ROOT}/include"
    PRIVATE "${RL_CLIENT_ROOT}/src"
)
target_link_libraries(rclient PUBLIC OpenSSL::Crypto ws2_32 dnsapi)

add_executable(win32-example main.c)
target_compile_features(win32-example PRIVATE c_std_11)

# Build the repository's protocol-aware fixture with the same compiler and C
# runtime as the native example. The MSVC behavioral job can therefore test the
# PE directly without maintaining a second responder implementation.
add_executable(r-test-responder
    "${RL_CLIENT_ROOT}/tools/r_test_responder.c"
    "${RL_CLIENT_ROOT}/tools/r_test_responder_protocol.c"
)
target_compile_features(r-test-responder PRIVATE c_std_11)
target_include_directories(r-test-responder PRIVATE
    "${RL_CLIENT_ROOT}/include"
    "${RL_CLIENT_ROOT}/src"
    "${RL_CLIENT_ROOT}/tools"
)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(win32-example PRIVATE /W4 /WX)
    target_compile_options(r-test-responder PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(win32-example PRIVATE -Wall -Wextra -Werror)
    target_compile_options(r-test-responder PRIVATE -Wall -Wextra -Werror)
endif()
target_link_libraries(win32-example PRIVATE rclient)
target_link_libraries(r-test-responder PRIVATE rclient)
