#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"


def require(text, pattern, description):
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise AssertionError(f"release workflow lacks {description}")


def job(text, name):
    match = re.search(
        rf"^  {re.escape(name)}:\n(?P<body>.*?)(?=^  [a-z][a-z0-9_-]*:\n|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"release workflow lacks {name} job")
    return match.group("body")


def main():
    if not WORKFLOW.is_file():
        raise AssertionError("release workflow does not exist")
    text = WORKFLOW.read_text(encoding="utf-8")

    assert "pull_request_target" not in text
    require(text, r"^  pull_request:\s*$", "pull-request validation")
    require(text, r"^  workflow_dispatch:\s*$", "manual dry run")
    require(text, r"^  push:\s*$", "tag publication trigger")
    require(text, r"^\s+- 'v\*'\s*$", "version-tag filter")
    require(
        text,
        r"^permissions:\n  contents: read\s*$",
        "read-only default permissions",
    )

    uses = re.findall(r"^\s+uses:\s+([^#\s]+)", text, flags=re.MULTILINE)
    if not uses:
        raise AssertionError("release workflow uses no actions")
    for action in uses:
        if re.fullmatch(r"[^@\s]+@[0-9a-f]{40}", action) is None:
            raise AssertionError(f"action is not pinned to a commit: {action}")
    for required_action in (
        "actions/checkout",
        "actions/upload-artifact",
        "actions/download-artifact",
        "actions/attest-build-provenance",
    ):
        if not any(action.startswith(f"{required_action}@") for action in uses):
            raise AssertionError(f"release workflow lacks {required_action}")

    checkout_steps = re.findall(
        r"uses:\s+actions/checkout@[0-9a-f]{40}\n"
        r"(?P<configuration>(?:\s{8,}.+\n)*)",
        text,
    )
    assert checkout_steps
    for configuration in checkout_steps:
        if "persist-credentials: false" not in configuration:
            raise AssertionError("checkout must disable persisted credentials")

    metadata = job(text, "metadata")
    for token in (
        "GITHUB_EVENT_NAME",
        "GITHUB_REF_NAME",
        "fetch-depth: 0",
        "tools/verify_release_tag.sh",
        "manifest/release-signing-keys.asc",
        "origin/main",
        "git show -s --format=%ct HEAD",
        "0.0.0",
        "^[0-9]+\\.[0-9]+\\.[0-9]+$",
    ):
        assert token in metadata

    tag_verifier = (
        ROOT / "tools" / "verify_release_tag.sh"
    ).read_text(encoding="utf-8")
    for token in (
        "git cat-file -t",
        "git verify-tag",
        "git merge-base --is-ancestor",
        "GNUPGHOME",
    ):
        assert token in tag_verifier

    contracts = job(text, "contracts")
    for token in (
        "tests/test_release_workflow.py",
        "tests/test_release_docs.py",
        "tests/test_dependency_sbom.py",
        "tests/test_cmake_minimum.py",
        "tests/test_windows_x64_release.py",
        "tests/test_windows_arm64_release.py",
        "tests/test_shared_abi.sh",
        "tests/test_shared_only_install.sh",
    ):
        assert token in contracts

    source = job(text, "source")
    assert "tools/package_source.py" in source
    assert "tools/verify_source_archive.py" in source
    assert "tests/test_source_release.py" in source
    assert "tests/test_source_embed_direct.sh" in source
    assert "tests/test_source_embed_subdirectory.sh" in source

    linux = job(text, "linux")
    for token in (
        "ubuntu24.04",
        "debian13",
        "fedora44",
        "ubuntu-24.04-arm",
        "ubuntu-24.04",
        "packaging/linux/build-packages.sh",
        "packaging/linux/verify-packages.sh",
    ):
        assert token in linux

    macos = job(text, "macos")
    for token in (
        "macos-15",
        "macos-15-intel",
        "packaging/macos/build-sdk.sh",
    ):
        assert token in macos

    macos_universal = job(text, "macos_universal")
    assert "packaging/macos/create-universal-sdk.sh" in macos_universal
    assert "macos-aarch64" in macos_universal
    assert "macos-amd64" in macos_universal

    windows = job(text, "windows")
    for token in (
        "windows-2022",
        "windows-11-arm",
        "amd64",
        "aarch64",
        "40f3c709db80acf154ac4b17a1f83c564ebd022e",
        "packaging/windows/build-sdk.ps1",
    ):
        assert token in windows

    aggregate = job(text, "aggregate")
    for dependency in (
        "contracts",
        "source",
        "linux",
        "macos",
        "macos_universal",
        "windows",
    ):
        assert dependency in aggregate
    assert "tools/assemble_release.py" in aggregate
    assert "release-assets" in aggregate

    attest = job(text, "attest")
    assert "refs/tags/v" in attest
    assert "github.event_name == 'push'" in attest
    assert "id-token: write" in attest
    assert "attestations: write" in attest
    assert "subject-path" in attest

    publish = job(text, "publish")
    assert "refs/tags/v" in publish
    assert "github.event_name == 'push'" in publish
    assert re.search(r"^\s+- metadata\s*$", publish, re.MULTILINE)
    assert "VERSION: ${{ needs.metadata.outputs.version }}" in publish
    assert '${TAG#v}' in publish
    assert "GH_REPO: ${{ github.repository }}" in publish
    assert "contents: write" in publish
    assert "gh release create" in publish
    assert "--draft" in publish
    assert "RELEASE-MANIFEST.json" in publish
    assert "SHA256SUMS" in publish
    assert "gh release edit" in publish
    assert "--draft=false" in publish

    assert "secrets." not in text
    assert "write-all" not in text

    linux_verifier = (
        ROOT / "packaging" / "linux" / "verify-packages.sh"
    ).read_text(encoding="utf-8")
    for token in (
        "dpkg-deb --contents",
        "dpkg-deb --field",
        "rpm -qlp",
        "rpm -qp --requires",
        "rclient-config.cmake",
        "rclient.pc",
    ):
        assert token in linux_verifier

    macos_builder = (
        ROOT / "packaging" / "macos" / "build-sdk.sh"
    ).read_text(encoding="utf-8")
    for token in (
        "manifest/public-api.symbols",
        "pkg-config --cflags --libs rclient",
        "tests/fixtures/installed_consumer",
    ):
        assert token in macos_builder

    print("test_release_workflow: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
