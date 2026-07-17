cmake_minimum_required(VERSION 3.16)
project(rl_llhttp_example C)

set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
set(LLHTTP_ROOT "" CACHE PATH "Optional llhttp installation prefix")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LLHTTP QUIET IMPORTED_TARGET libllhttp)
endif()
if(TARGET PkgConfig::LLHTTP)
    set(LLHTTP_TARGET PkgConfig::LLHTTP)
else()
    find_path(LLHTTP_INCLUDE_DIR llhttp.h HINTS "${LLHTTP_ROOT}/include")
    find_library(LLHTTP_LIBRARY llhttp HINTS
        "${LLHTTP_ROOT}/lib" "${LLHTTP_ROOT}/lib64")
    if(NOT LLHTTP_INCLUDE_DIR OR NOT LLHTTP_LIBRARY)
        message(FATAL_ERROR
            "llhttp not found; set PKG_CONFIG_PATH or LLHTTP_ROOT")
    endif()
    add_library(llhttp_vendor UNKNOWN IMPORTED)
    set_target_properties(llhttp_vendor PROPERTIES
        IMPORTED_LOCATION "${LLHTTP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LLHTTP_INCLUDE_DIR}")
    set(LLHTTP_TARGET llhttp_vendor)
endif()

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
add_executable(llhttp-example main.c llhttp_adapter.c)
target_compile_features(llhttp-example PRIVATE c_std_11)
if(MSVC)
    target_compile_options(rclient PRIVATE /W4 /WX)
    target_compile_options(llhttp-example PRIVATE /W4 /WX)
else()
    target_compile_options(rclient PRIVATE -Wall -Wextra -Werror)
    target_compile_options(llhttp-example PRIVATE -Wall -Wextra -Werror)
endif()
target_include_directories(llhttp-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(llhttp-example PRIVATE
    rclient ${LLHTTP_TARGET} OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(llhttp-example PRIVATE ws2_32 dnsapi)
else()
    target_link_libraries(llhttp-example PRIVATE resolv)
endif()
