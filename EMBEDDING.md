# Embedding rl-c-client

The source release contains only production sources, public/private headers,
build metadata, and essential documentation. It intentionally excludes tests,
examples, responders, CI configuration, and generated binaries.

OpenSSL libcrypto remains an external dependency. POSIX builds also link the
resolver and thread libraries; Windows builds link Winsock and DNS APIs.

## CMake subdirectory

```cmake
set(RCLIENT_BUILD_TESTS OFF)
set(RCLIENT_BUILD_EXAMPLES OFF)
set(RCLIENT_ENABLE_INSTALL OFF)
add_subdirectory(vendor/rl-c-client)
target_link_libraries(my_app PRIVATE rclient::rclient)
```

The project uses namespaced options and does not change parent compiler flags,
the parent C standard, or parent install rules.

## Direct source compilation

The bundle's `cmake/rclient-embed.cmake` reads the canonical source manifests
and provides absolute paths and platform dependencies:

```cmake
include(vendor/rl-c-client/cmake/rclient-embed.cmake)
target_sources(my_app PRIVATE ${RCLIENT_EMBED_SOURCES})
target_include_directories(my_app PRIVATE ${RCLIENT_EMBED_INCLUDE_DIRS})
target_link_libraries(my_app PRIVATE ${RCLIENT_EMBED_LIBRARIES})
```

The `manifest/*.sources` files remain available as line-oriented inventories
for consumers that do not use CMake.

Direct Windows builds do not define `RCLIENT_SHARED`; this keeps public
declarations suitable for compiling directly into an executable.
