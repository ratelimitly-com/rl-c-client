# Releasing rl-c-client

Releases are built only by `.github/workflows/release.yml`. Maintainers create
the version tag; the workflow derives every archive and package from that
immutable commit. Supported release tags are exactly `vMAJOR.MINOR.PATCH`
without prerelease or build suffixes.

## Validate before tagging

Use the workflow's `workflow_dispatch` trigger with a numeric version to run the
complete matrix without creating a GitHub release. Pull requests that change
release inputs run the same non-publishing matrix with version `0.0.0`.

The matrix builds on the target architecture instead of cross-compiling:

- Ubuntu 24.04, Debian 13, and Fedora 44 on AMD64 and AArch64;
- macOS on Intel and Apple Silicon, followed by a universal2 merge; and
- Windows on x64 and ARM64 hosted runners.

Each job builds, runs its tests, audits the packaged ABI and dependencies, and
compiles a consumer against an extracted or installed package. The aggregation
job accepts exactly 19 payload artifacts: 12 Linux packages, three macOS SDKs,
two Windows SDKs, and two source archives. It then adds
`RELEASE-MANIFEST.json` and `SHA256SUMS`.

The macOS and Windows SDKs statically link OpenSSL. They include the exact
OpenSSL license used by the build and a deterministic SPDX dependency inventory
that records the bundled OpenSSL version and `STATIC_LINK` relationship.
The macOS builds compile pinned OpenSSL LTS sources for a declared macOS 12.0
deployment target and reject a different minimum in the resulting Mach-O.

## Windows compatibility policy

Windows releases use the pinned WDK NuGet toolchain version `10.0.26100.6584`,
MSVC toolset `14.44.35207`, and pinned vcpkg sources. Builds are performed on
the native target runner with the Visual Studio 2022 generator. The build fails
if CMake selects a different MSVC toolset, and records the actual compiler and
linker versions in `toolchain.json`. The ARM64 WDK package does not carry the
WDK SBOM executable, so that job runs the pinned x64 WDK SBOM tool under
Windows ARM64's x64 emulation and records both packages in `toolchain.json`.

The release DLL uses the MSVC MultiThreaded (`/MT`) runtime and statically
links OpenSSL. Verification rejects imports from the dynamic UCRT,
VCRuntime, MSVC++, and OpenSSL DLL families and checks the PE machine and public
export table. This removes Visual C++ Redistributable and OpenSSL DLL
installation as deployment requirements. It does not claim compatibility with
unsupported Windows versions; the supported Windows API surface and target
architecture still apply.

## Publish

Start from an up-to-date, clean `main`, and create a signed annotated tag:

```sh
git switch main
git pull --ff-only origin main
git status --short
git tag -s vMAJOR.MINOR.PATCH -m 'rl-c-client vMAJOR.MINOR.PATCH'
git push origin vMAJOR.MINOR.PATCH
```

The workflow accepts signatures only from the public keys committed in
`manifest/release-signing-keys.asc`. The current allowed primary fingerprint is
`EFECDDA47F063AA02A1042DEC8F243431345A599`. Key changes require normal review
before a release tag is created.

The tag workflow:

1. validates the tag and records its commit and commit timestamp;
2. builds and verifies all 19 payload artifacts;
3. rejects any missing, duplicate, or unexpected asset;
4. generates the release manifest and checksums;
5. creates a GitHub build-provenance attestation for every release asset;
6. creates or resumes a draft GitHub release and uploads the exact asset set;
7. compares every draft asset's remote name and SHA-256 digest with the local
   set; and
8. changes the release from draft to public only after the comparison passes.

A failed publication stays in draft. A rerun may resume that draft and replace
its expected assets, but the workflow refuses to overwrite an already
published release. Inspect the workflow run, the draft assets, and the
attestation results before retrying.

## Verify the published result

Download `SHA256SUMS` with all release assets and run:

```sh
sha256sum --check SHA256SUMS
gh attestation verify rl-c-client-vMAJOR.MINOR.PATCH-source.tar.gz \
  --repo ratelimitly-com/rl-c-client
```

Confirm that the release page contains the 19 payload artifacts plus
`RELEASE-MANIFEST.json` and `SHA256SUMS`, and that the release points to the
signed tag.
