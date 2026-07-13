# Contributing

This project is released under the MIT License. Contributions should keep the
public API small, documented, and usable from a clean checkout.

## Development Setup

Install:

- C11 compiler
- `make`
- OpenSSL development headers and libcrypto

Run:

```sh
make clean
make
make test
make perf_client
```

## Public API Rule

Consumers must include only:

- `include/r_client.h`
- `include/r_client_io.h`

Do not add examples, tests, or downstream integrations that include `src/*.h`.
If an integration needs something from `src/`, promote a narrow wrapper to the
public API instead.

## Documentation

Public docs should explain the C API and integration behavior using only files
in this repository. If a change exposes behavior that users need to rely on,
document that behavior here at the API level.

## Pull Requests

Before opening a pull request, run:

```sh
make clean
make test
make perf_client
```

Keep changes scoped. API changes should include tests and documentation updates.
