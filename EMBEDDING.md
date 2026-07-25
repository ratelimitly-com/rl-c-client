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

The `manifest/*.sources` files are line-oriented source inventories. Compile
all entries from `core.sources`, `workflow.sources`, and `runtime.sources`, add
`include/` and `src/` to the private include path, then link platform
dependencies listed above.

Direct Windows builds do not define `RCLIENT_SHARED`; this keeps public
declarations suitable for compiling directly into an executable.
