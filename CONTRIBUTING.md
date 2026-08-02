# Contributing

This project is released under the MIT License. Contributions should keep the
public API small, documented, and usable from a clean checkout.

## Development Setup

Install:

- C11 compiler
- `make` and `bash`
- OpenSSL development headers and libcrypto
- `python3` (required by the documentation-quality checks in `make test`)

Run:

```sh
make clean
make
make test
make perf_client
```

## Public API Rule

Consumers must include only the public headers under `include/`:

- `include/r_client.h` and `include/r_client_io.h` (core)
- `include/r_client_workflow.h` (optional admission workflow)
- `include/r_client_runtime.h` (optional public runtime)

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
