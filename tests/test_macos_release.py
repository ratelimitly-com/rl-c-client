#!/usr/bin/env python3
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    config = json.loads(
        (ROOT / "manifest" / "macos-release.json").read_text(
            encoding="utf-8"
        )
    )
    assert config == {
        "deployment_target": "12.0",
        "openssl": {
            "sha256": (
                "a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8"
            ),
            "url": (
                "https://github.com/openssl/openssl/releases/download/"
                "openssl-3.5.7/openssl-3.5.7.tar.gz"
            ),
            "version": "3.5.7",
        },
    }

    openssl_builder = (
        ROOT / "packaging" / "macos" / "build-openssl.sh"
    ).read_text(encoding="utf-8")
    for token in (
        "MACOSX_DEPLOYMENT_TARGET",
        "darwin64-arm64-cc",
        "darwin64-x86_64-cc",
        "shasum -a 256",
        "./Configure",
        "no-shared",
        "install_sw",
        "libcrypto.a",
    ):
        assert token in openssl_builder

    sdk_builder = (
        ROOT / "packaging" / "macos" / "build-sdk.sh"
    ).read_text(encoding="utf-8")
    for token in (
        "manifest/macos-release.json",
        "CMAKE_OSX_DEPLOYMENT_TARGET",
        "LC_BUILD_VERSION",
        "minos",
    ):
        assert token in sdk_builder

    workflow = (
        ROOT / ".github" / "workflows" / "release.yml"
    ).read_text(encoding="utf-8")
    assert "packaging/macos/build-openssl.sh" in workflow
    assert "brew install openssl@3" not in workflow

    print("test_macos_release: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
