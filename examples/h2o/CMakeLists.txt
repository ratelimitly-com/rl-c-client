cmake_minimum_required(VERSION 3.16)
project(rl_h2o_example C)

if(WIN32)
    message(FATAL_ERROR "H2O's embedded evloop example targets Linux and macOS")
endif()
set(RL_CLIENT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the rl-c-client source tree")
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(H2O REQUIRED IMPORTED_TARGET libh2o-evloop)

add_library(rclient STATIC IMPORTED)
set_target_properties(rclient PROPERTIES
    IMPORTED_LOCATION "${RL_CLIENT_ROOT}/librclient.a")
add_executable(h2o-example main.c)
target_compile_features(h2o-example PRIVATE c_std_11)
target_compile_options(h2o-example PRIVATE -Wall -Wextra -Werror)
target_include_directories(h2o-example PRIVATE "${RL_CLIENT_ROOT}/include")
target_link_libraries(h2o-example PRIVATE
    rclient PkgConfig::H2O OpenSSL::Crypto Threads::Threads resolv)
