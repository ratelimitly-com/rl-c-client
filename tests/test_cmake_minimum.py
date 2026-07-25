#!/usr/bin/env python3
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    packaging = (ROOT / "cmake" / "rclient-packaging.cmake").read_text(
        encoding="utf-8"
    )
    assert re.search(
        r"^cmake_minimum_required\(VERSION 3[.]20\)$",
        cmake,
        re.MULTILINE,
    )
    assert "PROJECT_IS_TOP_LEVEL" not in cmake
    assert "PROJECT_IS_TOP_LEVEL" not in packaging
    assert "RCLIENT_IS_TOP_LEVEL" in cmake
    assert "RCLIENT_IS_TOP_LEVEL" in packaging
    print("test_cmake_minimum: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
