# C r-client (MVP)

This is a C implementation of the Ratelimitly r-client. It follows the
protocol specs in `docs/spec/r-client.md` and `docs/spec/wire_protocol.md`.

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

## Test

Run the unit tests:

```sh
make test
```

Clean build artifacts:

```sh
make clean
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
