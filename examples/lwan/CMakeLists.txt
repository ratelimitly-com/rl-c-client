cmake_minimum_required(VERSION 3.16)
project(rl_lwan_example C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Lwan and this coroutine bridge target Linux")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
set(LWAN_ROOT "" CACHE PATH "Path to the Lwan source tree")
set(LWAN_BUILD "${LWAN_ROOT}/build" CACHE PATH "Path to the Lwan build tree")
set(LWAN_LIBRARY "${LWAN_BUILD}/src/lib/liblwan.a")
if(NOT EXISTS "${LWAN_LIBRARY}")
    message(FATAL_ERROR "Build Lwan first and set LWAN_ROOT and LWAN_BUILD")
endif()
if(NOT EXISTS "${LWAN_BUILD}/lwan.pc")
    message(FATAL_ERROR "Lwan build metadata is missing: ${LWAN_BUILD}/lwan.pc")
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# Lwan records optional Brotli, Zstd, Lua, and allocator dependencies in the
# pkg-config file generated for that exact build. Keep the explicit static
# archive below for whole-archive linking, and use the metadata only for its
# dependency list.
set(_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "${LWAN_BUILD}:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(LWAN_BUILD_CONFIG REQUIRED lwan)
set(ENV{PKG_CONFIG_PATH} "${_saved_pkg_config_path}")
set(LWAN_DEP_LIBS ${LWAN_BUILD_CONFIG_LDFLAGS})
list(FILTER LWAN_DEP_LIBS EXCLUDE REGEX "^-L")
list(REMOVE_ITEM LWAN_DEP_LIBS -llwan)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(lwan-example main.c)
target_compile_features(lwan-example PRIVATE c_std_11)
target_compile_options(lwan-example PRIVATE
    -Wall -Wextra -Werror "-include${LWAN_BUILD}/lwan-build-config.h")
target_include_directories(lwan-example PRIVATE
    "${RL_CLIENT_ROOT}/include" "${LWAN_ROOT}/src/lib")
target_link_directories(lwan-example PRIVATE
    ${LWAN_BUILD_CONFIG_LIBRARY_DIRS})
target_link_libraries(lwan-example PRIVATE
    rclient "-Wl,--whole-archive" "${LWAN_LIBRARY}" "-Wl,--no-whole-archive"
    ${LWAN_DEP_LIBS} OpenSSL::Crypto Threads::Threads resolv dl m)
