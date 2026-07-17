# Mongoose HTTP integration

This self-contained folder shows a single-threaded Mongoose server. `GET
/limited` creates pending admission state; each short `mg_mgr_poll` tick is
followed by nonblocking client socket and deadline processing.

The request contains a resource rate limit and a latency guard. Resource denial
returns 429, latency shedding returns 503, and an allowed response is measured
and reported after Mongoose queues it. `MG_EV_CLOSE` cancels abandoned work.

## Build

Download or check out Mongoose and point `MONGOOSE_ROOT` at the directory that
contains `mongoose.c` and `mongoose.h`:

```sh
make -C ../..
make MONGOOSE_ROOT=/path/to/mongoose
./mongoose-example
```

```sh
cmake -S . -B build -DMONGOOSE_ROOT=/path/to/mongoose
cmake --build build
./build/mongoose-example
```

Set `RATELIMITLY_TENANT` and `RATELIMITLY_AUTH_KEY`, then request
`http://127.0.0.1:8000/limited`.

## Platform support

Mongoose and the public runtime support Linux, macOS, and Windows. The build
files add WinSock/DNS libraries on Windows and the resolver library on Unix.

## Ownership and production use

The `mg_mgr_poll` thread owns connections, pending nodes, request storage, and
the runtime. Unlink pending state before replying because a reply can schedule
connection teardown. Save `next` before timeout processing because completion
can free the current node. A 10 ms manager tick bounds client UDP latency in
this compact integration; a production host may integrate its own fd watcher.
