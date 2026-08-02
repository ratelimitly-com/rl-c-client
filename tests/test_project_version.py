#!/usr/bin/env python3
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_VERSION = "0.5.0"


def main():
    version_file = ROOT / "VERSION"
    assert version_file.read_text(encoding="utf-8") == (
        f"{EXPECTED_VERSION}\n"
    )

    cmake_lists = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'set(RCLIENT_VERSION_DEFAULT "0.0.0")' not in cmake_lists
    assert re.search(
        r'file\(STRINGS "\$\{CMAKE_CURRENT_SOURCE_DIR\}/VERSION"',
        cmake_lists,
    )

    with tempfile.TemporaryDirectory(prefix="rl-project-version-") as tmp:
        build = Path(tmp) / "build"
        subprocess.run(
            [
                "cmake",
                "-S",
                str(ROOT),
                "-B",
                str(build),
                "-DRCLIENT_BUILD_TESTS=OFF",
            ],
            check=True,
        )
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        assert (
            f"CMAKE_PROJECT_VERSION:STATIC={EXPECTED_VERSION}" in cache
        )
        assert f"RCLIENT_VERSION:STRING={EXPECTED_VERSION}" in cache

    print("test_project_version: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
