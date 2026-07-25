#!/usr/bin/env python3
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def require(path, *phrases):
    text = (ROOT / path).read_text(encoding="utf-8")
    for phrase in phrases:
        if phrase not in text:
            raise AssertionError(f"{path} does not document {phrase!r}")


def main():
    require(
        "README.md",
        "## Install a release",
        "ubuntu24.04",
        "debian13",
        "fedora44",
        "macos-universal2",
        "windows-amd64",
        "windows-aarch64",
        "source.tar.gz",
        "SHA256SUMS",
        "gh attestation verify",
        "EMBEDDING.md",
    )
    require(
        "EMBEDDING.md",
        "rl-c-client-v<VERSION>-source.tar.gz",
        "SOURCE-MANIFEST.json",
        "add_subdirectory",
        "RCLIENT_EMBED_SOURCES",
        "OpenSSL libcrypto",
    )
    require(
        "SECURITY.md",
        "## Release Integrity",
        "SHA256SUMS",
        "RELEASE-MANIFEST.json",
        "GitHub build-provenance attestation",
    )
    require(
        "RELEASING.md",
        "merge to `main`",
        "canonical `VERSION`",
        "vMAJOR.MINOR.PATCH",
        "workflow_dispatch",
        "already published",
        "19 payload artifacts",
        "10.0.26100.6584",
        "MultiThreaded (`/MT`)",
        "draft",
        "attest",
    )
    releasing = (ROOT / "RELEASING.md").read_text(encoding="utf-8")
    assert "create a signed annotated tag" not in releasing
    assert "version `0.0.0`" not in releasing
    require(
        "CHANGES.md",
        "native Ubuntu, Debian, Fedora, macOS, and Windows release artifacts",
        "embeddable source archives",
    )
    print("test_release_docs: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
