# Contributing

This repository is being prepared for public release. Until the license is
confirmed, external contributions should be limited to discussion and review.

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

Public docs should explain the C API and integration behavior. Do not copy the
private full wire protocol into this repository.

## Pull Requests

Before opening a pull request, run:

```sh
make clean
make test
make perf_client
```

Keep changes scoped. API changes should include tests and documentation updates.
