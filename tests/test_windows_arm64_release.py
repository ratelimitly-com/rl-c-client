#!/usr/bin/env python3
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    config = json.loads(
        (ROOT / "manifest" / "windows-release.json").read_text(
            encoding="utf-8"
        )
    )
    arm64 = config["architectures"]["aarch64"]
    assert arm64 == {
        "cmake_architecture": "ARM64",
        "msvc_target": "arm64",
        "nuget_package": "Microsoft.Windows.WDK.ARM64",
        "runner": "windows-11-arm",
        "runtime_architecture": "Arm64",
        "sbom_nuget_package": "Microsoft.Windows.WDK.x64",
        "sbom_tool": "sbom-tool-win-x64.exe",
        "vcpkg_triplet": "arm64-windows-static",
    }
    amd64 = config["architectures"]["amd64"]
    assert amd64["sbom_nuget_package"] == amd64["nuget_package"]

    build_script = (
        ROOT / "packaging" / "windows" / "build-sdk.ps1"
    ).read_text(encoding="utf-8")
    verify_script = (
        ROOT / "packaging" / "windows" / "verify-sdk.ps1"
    ).read_text(encoding="utf-8")
    assert '[ValidateSet("amd64", "aarch64")]' in build_script
    assert '[ValidateSet("amd64", "aarch64")]' in verify_script
    assert "sbom_nuget_package" in build_script
    assert "windows-11-arm" in json.dumps(config)
    assert "vcpkg_triplet" in build_script

    print("test_windows_arm64_release: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
