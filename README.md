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
