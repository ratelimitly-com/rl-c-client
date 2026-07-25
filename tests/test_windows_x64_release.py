#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.2.3"
COMMIT = "0123456789abcdef0123456789abcdef01234567"


def main():
    config = json.loads(
        (ROOT / "manifest" / "windows-release.json").read_text(
            encoding="utf-8"
        )
    )
    assert config["wdk_version"] == "10.0.26100.6584"
    assert config["msvc_toolset_version"] == "14.44.35207"
    assert config["vcpkg_commit"] == "40f3c709db80acf154ac4b17a1f83c564ebd022e"
    amd64 = config["architectures"]["amd64"]
    assert amd64 == {
        "cmake_architecture": "x64",
        "msvc_target": "x64",
        "nuget_package": "Microsoft.Windows.WDK.x64",
        "runner": "windows-2022",
        "runtime_architecture": "X64",
        "sbom_nuget_package": "Microsoft.Windows.WDK.x64",
        "sbom_tool": "sbom-tool-win-x64.exe",
        "vcpkg_triplet": "x64-windows-static",
    }
    assert config["forbidden_import_patterns"] == [
        "^api-ms-win-crt-",
        "^libcrypto",
        "^libssl",
        "^msvcp[0-9]",
        "^ucrtbase[.]dll$",
        "^vcruntime[0-9]",
    ]

    build_script = (
        ROOT / "packaging" / "windows" / "build-sdk.ps1"
    ).read_text(encoding="utf-8")
    verify_script = (
        ROOT / "packaging" / "windows" / "verify-sdk.ps1"
    ).read_text(encoding="utf-8")
    assert "RCLIENT_USE_STATIC_MSVC_RUNTIME=ON" in build_script
    assert "v143,host=x64,version=$($config.msvc_toolset_version)" in build_script
    assert "CMAKE_VS_PLATFORM_TOOLSET_VERSION" in build_script
    assert (
        '"^CMAKE_VS_PLATFORM_TOOLSET_VERSION:[^=]+=(.+)$"'
        in build_script
    )
    assert "compiler_version" in build_script
    assert "linker_version" in build_script
    assert "git -C $resolvedVcpkgRoot rev-parse HEAD" in build_script
    assert "$actualVcpkgCommit -ne $config.vcpkg_commit" in build_script
    assert "vcpkg_commit = $actualVcpkgCommit" in build_script
    assert "RCLIENT_BUNDLE_OPENSSL=ON" in build_script
    assert "nuget install" in build_script
    assert "sbomtool" in build_script.lower()
    assert "dumpbin" in verify_script.lower()
    assert "public-api.symbols" in verify_script
    assert "Compress-Archive" not in build_script

    with tempfile.TemporaryDirectory(prefix="rl-windows-zip-") as tmp:
        tmp_path = Path(tmp)
        stage = tmp_path / "stage"
        output_a = tmp_path / "a"
        output_b = tmp_path / "b"
        (stage / "bin").mkdir(parents=True)
        (stage / "include").mkdir()
        (stage / "bin" / "rclient.dll").write_bytes(b"fixture-dll")
        (stage / "include" / "r_client.h").write_bytes(b"fixture-header")
        env = os.environ.copy()
        env["SOURCE_DATE_EPOCH"] = "1700000000"
        command = [
            sys.executable,
            str(ROOT / "tools" / "package_sdk.py"),
            "--stage",
            str(stage),
            "--version",
            VERSION,
            "--commit",
            COMMIT,
            "--platform",
            "windows",
            "--architecture",
            "amd64",
            "--archive-format",
            "zip",
        ]
        subprocess.run(command + ["--output", str(output_a)], check=True, env=env)
        subprocess.run(command + ["--output", str(output_b)], check=True, env=env)
        archive_name = f"rl-c-client-v{VERSION}-windows-amd64-sdk.zip"
        archive_a = output_a / archive_name
        archive_b = output_b / archive_name
        assert hashlib.sha256(archive_a.read_bytes()).digest() == (
            hashlib.sha256(archive_b.read_bytes()).digest()
        )
        with zipfile.ZipFile(archive_a) as archive:
            names = sorted(archive.namelist())
            assert names == [
                f"rl-c-client-{VERSION}-windows-amd64/SDK-MANIFEST.json",
                f"rl-c-client-{VERSION}-windows-amd64/bin/rclient.dll",
                f"rl-c-client-{VERSION}-windows-amd64/include/r_client.h",
            ]

    print("test_windows_x64_release: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
