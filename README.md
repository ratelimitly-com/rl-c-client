# C r-client (MVP)

This is a C implementation of the Ratelimitly r-client. It follows the
protocol specs in `docs/spec/r-client.md` and `docs/spec/wire_protocol.md`.
Auth for `cookie`/`aes` uses tenant Bech32 keys (`rl-cookie...`, `rl-aes...`) with embedded quotas rather than passphrase strings.

## Build

Requirements:
- C compiler (C11)
- OpenSSL development headers and libcrypto
- make

Build static + shared libraries:

```sh
make
```

Outputs:
- `librclient.a`
- `librclient.so`

Build perf client binary:

```sh
make perf_client
```

Output:
- `bin/perf_client`

## Test

Run the unit tests:

```sh
make test
```

Clean build artifacts:

```sh
make clean
```

## Perf client

Run the performance client (mirrors `clients/rust/bin/perf_client.rs`):

```sh
bin/perf_client --clients=50 --requests=10000
```

Examples:

```sh
bin/perf_client --duration=60 --auth=rl-aes1...
bin/perf_client --srv=rl1.glar.com --duration=30 --clients=50
RCLIENT_DNS_SERVER=127.0.0.1:5353 bin/perf_client
```

## Notes

- The API is defined in `clients/c/include/r_client.h`.
- I/O integration is defined in `clients/c/include/r_client_io.h` and
  described in `clients/c/IO_ABSTRACTION.md`.

## Borrowed-buffer usage

If you want to avoid per-request allocations, you can use the borrowed API.
In this mode, the client does not copy input buffers; the caller must keep
them alive until the callback fires.

Example:

```c
static void on_rate(void *user, r_client_req_t *req, int status, const r_rate_limit_result_t *res) {
    (void)user; (void)req; (void)status; (void)res;
}

void submit_request(r_client_t *client) {
    r_resource_request_t resources[2];
    memset(resources, 0, sizeof(resources));
    // Fill resources...

    const char *label = "api:$method:$uri";

    r_client_check_rate_limit_async_borrowed(
        client,
        resources, 2,
        NULL, 0,
        label, 0,
        on_rate, NULL,
        NULL
    );
    // resources and label must remain valid until on_rate is called.
}
```
