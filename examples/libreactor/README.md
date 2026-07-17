# libreactor v3 integration

This self-contained example serves `GET /limited` with libreactor v3. The loop
watches duplicates of the runtime-owned UDP sockets and arms one reactor timer
for the nearest admission deadline. Each request combines a resource rate limit
with a latency guard.

Allowed requests run a protected operation, measure it monotonically, and
report that sample before responding. Replace `perform_protected_work()` with
the database query, RPC, or other operation the endpoint should protect.

## Build and run

Build and install libreactor v3 so `pkg-config libreactor` can find it, then:

```sh
make -C ../..
make
RATELIMITLY_TENANT=example \
RATELIMITLY_AUTH_KEY=secret \
./libreactor-example
curl -i http://127.0.0.1:8000/limited
```

For a non-system install, set `PKG_CONFIG_PATH` to its `lib/pkgconfig` folder.
The equivalent CMake build is:

```sh
cmake -S . -B build
cmake --build build
./build/libreactor-example
```

## Decision mapping

- `200`: admitted; protected work completed and latency was reported.
- `429`: rejected by the resource limit, alone or with the latency guard.
- `503`: rejected only by latency, or admission infrastructure failed.

Denied requests never run or report protected work.

## Reentrancy and ownership

The reactor thread owns runtime and request state. libreactor descriptor objects
close their descriptors, so watchers use `dup()` and the runtime retains the
original. Both descriptors observe the same UDP receive queue; only the runtime
drains it.

Admission can complete synchronously while start or timeout code is still on
the stack. `defer_completion` records that callback result and delays destruction
until the owning operation returns. Removing this guard creates a reentrant
use-after-free. libreactor retains the server request until `server_respond()`,
including after a peer disconnect.

## Platform support

libreactor v3 is Linux-focused and this example is tested on Linux. CMake fails
clearly on other systems. Use Mongoose, libevent, or another portable example
for macOS and Windows hosts.
