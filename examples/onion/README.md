# Onion yielded-response bridge

This self-contained example serves `/limited` using Onion's long-poll
`OCS_YIELD` lifecycle. HTTP workers transfer request/response ownership to one
bridge thread, which owns the runtime, UDP readiness, deadlines, and combined
resource/latency admission.

Allowed requests run protected application work, measure it monotonically, and
report the sample before the bridge completes HTTP. Replace
`prepare_protected_response()` with the database query, RPC, or other operation
the route should protect.

## Build and run

Build and install Onion v0.8 to a prefix, then pass that prefix to the example:

```sh
git clone --depth 1 --branch v0.8 \
  https://github.com/davidmoreno/onion.git /tmp/onion-v0.8
cmake -S /tmp/onion-v0.8 -B /tmp/onion-build \
  -DCMAKE_INSTALL_PREFIX=/tmp/onion-install
cmake --build /tmp/onion-build
cmake --install /tmp/onion-build
make -C ../..
make ONION_ROOT=/tmp/onion-install
RATELIMITLY_TENANT=example \
RATELIMITLY_AUTH_KEY=secret \
./onion-example
curl -i http://127.0.0.1:8000/limited
```

Or use CMake:

```sh
cmake -S . -B build -DONION_ROOT=/tmp/onion-install
cmake --build build
./build/onion-example
```

## Decision mapping

- `200`: admitted; protected work completed and latency was reported.
- `429`: rejected by the resource limit, alone or with the latency guard.
- `503`: rejected only by latency, or admission infrastructure failed.

Denied requests never run or report protected work.

## Yielded ownership and shutdown

Returning `OCS_YIELD` transfers each request and response to its heap job. The
bridge writes and flushes the response, then frees both Onion objects exactly
once. A disconnected peer can make `onion_response_flush()` fail, but ownership
and cleanup are unchanged.

Only the bridge enters rl-c-client. On shutdown or loop failure it cancels
active admission and completes every queued/yielded response with an error so
no worker remains suspended. Keep the bridge alive until `onion_listen()` has
stopped accepting work and Onion has completed its worker lifecycle.

## Platform support

This pthread/pipe/poll bridge is POSIX-portable and is validated with Onion
v0.8 on Linux. Onion v0.8 itself needs upstream compatibility fixes to build
with current macOS/Clang, although the example can consume a compatible
prebuilt macOS Onion library. CMake rejects Windows explicitly; use Mongoose or
the native Win32 example for that platform.
