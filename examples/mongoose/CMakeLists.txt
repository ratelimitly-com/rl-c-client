cmake_minimum_required(VERSION 3.16)
project(rl_mongoose_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(MONGOOSE_ROOT "" CACHE PATH "Directory containing mongoose.c and mongoose.h")
if(NOT EXISTS "${MONGOOSE_ROOT}/mongoose.c")
    message(FATAL_ERROR "Set MONGOOSE_ROOT to a Mongoose source directory")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

set(RL_CLIENT_SOURCES
    "${RL_CLIENT_ROOT}/src/r_client.c"
    "${RL_CLIENT_ROOT}/src/r_client_runtime.c"
    "${RL_CLIENT_ROOT}/src/r_client_workflow.c"
    "${RL_CLIENT_ROOT}/src/r_protocol.c"
    "${RL_CLIENT_ROOT}/src/r_crypto.c"
    "${RL_CLIENT_ROOT}/src/r_policy.c")
add_library(rclient STATIC ${RL_CLIENT_SOURCES})
target_compile_features(rclient PUBLIC c_std_11)
target_include_directories(rclient
    PUBLIC "${RL_CLIENT_ROOT}/include"
    PRIVATE "${RL_CLIENT_ROOT}/src")
add_library(mongoose_vendor STATIC "${MONGOOSE_ROOT}/mongoose.c")
target_include_directories(mongoose_vendor PUBLIC "${MONGOOSE_ROOT}")
if(NOT WIN32)
    target_compile_definitions(mongoose_vendor PRIVATE _GNU_SOURCE)
endif()
add_executable(mongoose-example main.c)
target_compile_features(mongoose-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(mongoose-example PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(mongoose-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(mongoose-example PRIVATE
    "${RL_CLIENT_ROOT}/include" "${MONGOOSE_ROOT}")
target_link_libraries(mongoose-example PRIVATE
    rclient mongoose_vendor OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(mongoose-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(mongoose-example PRIVATE resolv)
endif()
