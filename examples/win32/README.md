# Native Win32 event-loop integration

> **Prerequisites.** You can read C and know what a UDP socket and an event
> loop are. Building requires a Windows-targeting C compiler, Windows OpenSSL
> development files, and either Make or CMake; every Windows-specific term is
> explained here.

## TL;DR

This one-shot Win32 program submits exactly one admission per process,
combining one resource rate limit with one pre-work latency guard. Allowed work
runs once and reports one measured latency-tracker sample; denied, cancelled,
or failed work reports none.

## What this example teaches

The example builds a small event loop from native Windows Sockets 2 (WinSock)
event objects. `WSAEventSelect` associates each runtime-owned UDP `SOCKET` with
a `WSAEVENT`, `WSAWaitForMultipleEvents` waits for either readable network data
or the current admission timeout, and `WSAEnumNetworkEvents` consumes the
signaled event before the client drains datagrams.

It also shows the whole protected-operation lifecycle: submit one combined
resource-and-latency admission, run work only when both checks pass, measure
that successful work with a monotonic clock, report one duration to its latency
tracker, and release application-owned events before runtime-owned sockets.
[Example source](main.c)

The program is deliberately **one-shot**: `start_admission()` is called once,
so each process performs exactly one logical admission and then exits. A
long-running service would repeat the same lifecycle for each protected
operation while keeping one WinSock loop alive.

## Configuration and production discovery

`RATELIMITLY_AUTH_KEY` is required. The client authenticates its packets with
that key and decodes the tenant key ID from it. With no overrides, the key ID
produces the production P0 tenant name:

```text
c-<key-id>.p0.ratelimitly.com
```

The runtime then queries this service-location (SRV) record to discover the UDP
host and port:

```text
_ratelimitly._udp.c-<key-id>.p0.ratelimitly.com
```

`RATELIMITLY_TENANT` is an optional **DNS-name override**; it does not replace
the identity or authentication material decoded from the key. Leave it unset
for key-derived production P0 discovery.

Local deterministic tests can bypass SRV discovery with a fixed endpoint.
`RATELIMITLY_EXAMPLE_SERVER_HOST` and
`RATELIMITLY_EXAMPLE_SERVER_PORT` form one pair: set **both or neither**.
Supplying only one is a configuration error.

```powershell
$env:RATELIMITLY_AUTH_KEY = "rl-aes1..."

# Optional local responder. Remove both variables for production P0.
$env:RATELIMITLY_EXAMPLE_SERVER_HOST = "127.0.0.1"
$env:RATELIMITLY_EXAMPLE_SERVER_PORT = "39082"

.\win32-example.exe
```

The environment contract is implemented in
[`r_runtime_options_from_env()`](../../src/r_client_runtime.c).
Never commit an authentication key or place it in a command-line argument.

## Control flow

```mermaid
flowchart TD
    Start["Start one resource + latency admission"] --> Events["Associate SOCKETs with WSAEVENTs"]
    Events --> Delay["Compute current admission delay"]
    Delay --> Wait["WSAWaitForMultipleEvents"]
    Wait --> Result{"Socket event or timeout?"}
    Result -->|FD_READ| Read["WSAEnumNetworkEvents, then drain datagrams"]
    Result -->|Timeout| Timeout["Advance admission timeout"]
    Read --> Decision{"Admission complete?"}
    Timeout --> Decision
    Decision -->|No| Delay
    Decision -->|Denied| Reject["Skip protected work and latency report"]
    Decision -->|Allowed| Work["Run work, measure it, report one sample"]
    Reject --> Close["Clear associations, close events, destroy runtime"]
    Work --> Close
```

`WSAEventSelect` makes its socket nonblocking and records only the requested
`FD_READ` event. Microsoft documents `WSAEnumNetworkEvents` as atomically
copying and clearing the network-event record and resetting the associated
event object, so a later read occurrence can signal it again. The loop
recomputes its timeout after every client transition because timeout handling
can change the active request deadline.

## Build and run with MinGW-w64

MinGW-w64 cross-compiles Windows Portable Executable (PE) files from Linux,
macOS, or Windows. Build a Windows `librclient.a` with the same MinGW toolchain
and a Windows OpenSSL build, then run this from the example directory:

```sh
make CC=x86_64-w64-mingw32-gcc \
  RL_CLIENT_LIBRARY=/path/to/windows/librclient.a \
  OPENSSL_PREFIX=/path/to/mingw/openssl
```

Run `win32-example.exe` directly on Windows or through Wine on Linux:

```sh
RATELIMITLY_AUTH_KEY=rl-aes1... wine64 ./win32-example.exe
```

On macOS, CrossOver can run the same PE in a 64-bit bottle:

```sh
wine --bottle <64-bit-bottle> --no-gui ./win32-example.exe
```

The OpenSSL prefix may contain either `lib/libcrypto.a` or
`lib64/libcrypto.a`. The [Makefile](Makefile) requires an explicit Windows
client archive, preventing an accidental native Unix library from entering the
link.

## Build and run with CMake and MSVC

Microsoft Visual C/C++ (MSVC) is the compiler shipped with Visual Studio. From
a PowerShell prompt, install the matching x64 Windows OpenSSL package and let
CMake compile the client and example with the selected compiler:

```powershell
vcpkg install openssl:x64-windows
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release

$env:RATELIMITLY_AUTH_KEY = "rl-aes1..."
.\build\Release\win32-example.exe
```

The CMake project in this folder compiles rl-c-client sources, the example, and
the protocol-aware test responder with the same compiler and C runtime.
That matters because a MinGW archive and an MSVC `.lib` do not share a
compatible application binary interface (ABI), object-library format, or C
runtime. MSVC builds use `/W4 /WX`, so compiler warnings fail the build.

## Expected output and exit status

The executable writes one outcome line after the admission callback completes.
These strings and return paths come directly from
[`print_outcome()` and `main()`](main.c):

| Outcome | Standard output | Standard error | Exit status |
|---|---|---|---:|
| Allowed | `allowed: inventory response prepared by Win32; latency=<milliseconds> ms` | empty | `0` |
| Resource rate limit denied | `denied: resource rate limit` | empty | `2` |
| Latency guard denied | `denied: latency guard` | empty | `2` |
| Both controls denied | `denied: resource limit and latency guard` | empty | `2` |
| Missing or invalid environment configuration | empty | `set RATELIMITLY_AUTH_KEY; RATELIMITLY_TENANT is optional` | `1` |
| Setup, transport, timeout, or protected-work failure | empty | `Win32 example failed: <status-name> (<status-code>)` | `1` |

An exit status of `2` is an expected policy decision, not a transport failure.
Protected response text appears only on the allowed path.

## Latency guard versus latency tracker

The **latency guard** is part of the pre-work admission request. It asks whether
the service's already-recorded latency state satisfies the configured
threshold. The **latency tracker** is that service state: it identifies and
retains prior duration samples using the tracker configuration carried by the
guard.

The current operation cannot influence its own guard decision. Only after both
the resource rate limit and guard allow the request does
`r_runtime_admission_run_and_report()` measure `prepare_response()` and submit
one new sample to the same tracker. The workflow rejects duplicate reports and
does not report denied, cancelled, or failed work; see the
[combined admission workflow](../../src/r_client_workflow.c) and
[runtime measurement helper](../../src/r_client_runtime.c).

## Adapting the synchronous demo to asynchronous work

`prepare_response()` only formats a short string, so the admission callback can
run it synchronously without meaningfully blocking this teaching loop. Real
disk, database, HTTP, or CPU-heavy work should not block the WinSock thread.

For asynchronous work:

1. On an allowed callback, record a monotonic start time and start the
   operation, but keep the application and admission-request storage alive.
2. Return to the WinSock loop instead of setting `done`.
3. Marshal successful completion back onto the loop thread, compute the
   monotonic duration, and call `r_client_admission_report_latency()` exactly
   once.
4. On failure or cancellation, finish without reporting a sample.
5. Stop the loop and destroy the runtime only after completion or cancellation
   has settled.

Keep client calls on one loop thread unless the application adds explicit
serialization. Reporting when work is merely *scheduled* measures queueing
setup, not the protected operation.

## Ownership and shutdown

The runtime owns WinSock startup and cleanup, the client handle, and every UDP
`SOCKET`. The application owns `WSAEVENT` objects, request storage, and its
copied outcome. Shutdown follows that ownership boundary:

1. Cancel an active request.
2. Clear each `WSAEventSelect` association.
3. Close every application-owned event.
4. Destroy the runtime and its sockets.

Never cast a `SOCKET` to `int`. Windows defines `SOCKET` as a pointer-width
handle, so such a cast can truncate it in a 64-bit process.

## Platform support and exact verification

The source intentionally requires `_WIN32`. It runs natively on Windows with
MSVC or MinGW-w64; Linux uses Wine and macOS can use CrossOver to execute the
Windows PE. Those compatibility runs do not make this a native Linux or macOS
event loop.

| Validation path | Environment | What the repository verifies | Evidence |
|---|---|---|---|
| MinGW-w64 build and Wine behavior | GitHub Ubuntu runner | Cross-builds x86-64 PE files with pinned static Windows OpenSSL; confirms no OpenSSL dynamic-library import; then checks allow, resource-denial, and latency-denial paths. Each process sends exactly one combined request; only allow emits one matching tracker report, and a post-exit drain catches late or duplicate reports. | [CI workflow](../../.github/workflows/ci.yml), [deterministic Wine test](../../tests/test_windows_example.sh) |
| Native MSVC behavior | GitHub Windows runner | Confirms CMake selected MSVC, builds with strict warnings, and runs the same three scenarios against an MSVC-built responder. It verifies the resource, guard, tracker fields, output, exit status, report count, and absence of late or duplicate reports. | [CI workflow](../../.github/workflows/ci.yml), [native PowerShell test](../../tests/test_windows_native_example.ps1) |
| Production P0 through Wine | Trusted `main` push or an `edescourtis` manual `main` run on Ubuntu | Rejects tenant and fixed-endpoint overrides, discovers P0 from the key, authenticates, receives admission, runs protected work, and completes the local latency-report send path within a hard deadline. | [Wine P0 test](../../tests/test_production_p0_win32_wine.sh) |
| Production P0 with native MSVC PE | Trusted `main` push or an `edescourtis` manual `main` run on Windows | Uses an explicit child environment containing only the key override, proves key-derived P0 discovery and admission, and requires one exact allowed/latency output line with empty standard error. | [native P0 PowerShell test](../../tests/test_production_p0_win32_example.ps1) |
| CrossOver behavior | Local macOS, when requested | The deterministic Wine test accepts a prebuilt PE plus CrossOver runner arguments, enabling the same three responder scenarios. This is a local path, not a current CI job. | [CrossOver-capable deterministic test](../../tests/test_windows_example.sh) |

Production admission is request/response, but the latency report is a
fire-and-forget UDP send. Therefore each production example smoke proves the
local report-send path, not server acknowledgement or storage of that report.

## Glossary

| Term | Meaning here |
|---|---|
| WinSock | Windows Sockets 2, Microsoft's native networking API used for `SOCKET`, `WSAEVENT`, and readiness calls. |
| CrossOver | CodeWeavers' commercial Wine-based product for running Windows applications on macOS and Linux without a Windows virtual machine. |
| SRV | DNS service-location record that returns a target host and port for a named service. |
| PE | Portable Executable, the Windows executable and object-container format; this example produces `win32-example.exe`. |
| CMake | Cross-platform build-system generator; here it creates a Visual Studio or MinGW build from this folder's project file. |
| MSVC | Microsoft Visual C/C++, the compiler and linker toolchain included with Visual Studio. |
| PowerShell | Microsoft's command shell and scripting language used by the native Windows build and tests. |
| ABI | Application binary interface: the calling, object-format, linking, and runtime conventions compiled components must agree on. |
| MinGW-w64 | Open-source compiler toolchain and Windows headers/libraries used to build Windows PE files. |
| Wine | Compatibility layer that runs Windows PE programs on Unix-like hosts; it is not a Windows emulator or native Linux build. |
| P0 | rl-c-client's default production DNS tier, encoded in the key-derived tenant name. |
| latency guard | Pre-work admission check against existing latency-tracker state. |
| latency tracker | Service-specific state that retains reported duration samples for later guard decisions. |

## API references

- [Example source](main.c)
- [Public example runtime API](../../include/r_client_runtime.h)
- [Combined admission workflow API](../../include/r_client_workflow.h)
- [Windows Sockets 2 overview](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-start-page-2)
- [Microsoft `SOCKET` type](https://learn.microsoft.com/en-us/windows/win32/winsock/socket-data-type-2)
- [Microsoft `WSAEventSelect` reference](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect)
- [Microsoft `WSAWaitForMultipleEvents` reference](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsawaitformultipleevents)
- [Microsoft `WSAEnumNetworkEvents` reference](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaenumnetworkevents)
- [Microsoft Portable Executable format specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
- [Microsoft vcpkg integration for CMake](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [Microsoft MSVC warning-level reference](https://learn.microsoft.com/en-us/cpp/build/reference/compiler-option-warning-level)
- [Microsoft PowerShell documentation](https://learn.microsoft.com/en-us/powershell/)
- [Microsoft SRV-record description](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-adts/c1987d42-1847-4cc9-acf7-aab2136d6952)
- [CodeWeavers CrossOver](https://www.codeweavers.com/crossover)
- [Wine project documentation](https://www.winehq.org/)
