cmake_minimum_required(VERSION 3.20)
project(rclient-installed-consumer LANGUAGES C)

find_package(rclient CONFIG REQUIRED)

add_executable(rclient-installed-consumer main.c)
target_link_libraries(rclient-installed-consumer PRIVATE rclient::rclient)
