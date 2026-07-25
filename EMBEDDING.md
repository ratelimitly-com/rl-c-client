# Embedding rl-c-client

Download `rl-c-client-v<VERSION>-source.tar.gz` or
`rl-c-client-v<VERSION>-source.zip` from the matching GitHub release. The
source release contains only production sources, public/private headers, build
metadata, and essential documentation. It intentionally excludes tests,
examples, responders, CI configuration, and generated binaries.

OpenSSL libcrypto remains an external dependency. POSIX builds also link the
resolver and thread libraries; Windows builds link Winsock and DNS APIs.

Each archive has a single `rl-c-client-<VERSION>` root. Its
`SOURCE-MANIFEST.json` records the exact source commit, version, normalized
build timestamp, size, and SHA-256 digest of every bundled input. Verify the
release asset against `SHA256SUMS` and its GitHub build-provenance attestation
before vendoring it.

## CMake subdirectory

Extract the archive below the application's source tree, for example at
`vendor/rl-c-client`, and make the dependency part of the parent build:

```cmake
set(RCLIENT_BUILD_TESTS OFF)
set(RCLIENT_ENABLE_INSTALL OFF)
add_subdirectory(vendor/rl-c-client)
target_link_libraries(my_app PRIVATE rclient::rclient)
```

The project uses namespaced options and does not change parent compiler flags,
the parent C standard, or parent install rules.

With MSVC, set `RCLIENT_USE_STATIC_MSVC_RUNTIME` before `add_subdirectory` when
the host and all of its static dependencies use the `MultiThreaded` (`/MT`)
runtime:

```cmake
set(RCLIENT_USE_STATIC_MSVC_RUNTIME ON)
add_subdirectory(vendor/rl-c-client)
```

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

Both integration modes intentionally keep OpenSSL libcrypto external. Link a
static OpenSSL build when the final executable must not depend on OpenSSL DLLs
or shared objects. Do not copy an ad hoc subset of `src/*.c`: the canonical
manifests are the compatibility boundary and keep the core, workflow, and
runtime source sets synchronized.
