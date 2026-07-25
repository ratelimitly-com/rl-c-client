#!/usr/bin/env python3
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def main():
    for relative in (
        "CMakeLists.txt",
        "EMBEDDING.md",
        "tests/test_source_embed_subdirectory.sh",
    ):
        content = (ROOT / relative).read_text(encoding="utf-8")
        assert "RCLIENT_BUILD_EXAMPLES" not in content, relative
    print("test_cmake_options: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
