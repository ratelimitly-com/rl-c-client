#!/usr/bin/env python3
"""Adversarial checks for the README Mermaid source gate."""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

from test_readme_quality import check_mermaid_block


def require_issue(name: str, block: str, expected: str) -> None:
    errors = check_mermaid_block(block, 1)
    if not any(expected in error for error in errors):
        raise AssertionError(f"{name}: expected {expected!r}, got {errors!r}")


def main() -> int:
    require_issue("empty block", "", "empty")
    require_issue(
        "frontmatter theme",
        "---\nconfig:\n  theme: dark\n---\nflowchart TD\n  A --> B",
        "forces a theme color",
    )
    require_issue(
        "initialization directive",
        "%%{init: {'theme': 'dark'}}%%\nflowchart TD\n  A --> B",
        "forces a theme color",
    )
    require_issue(
        "missing text color",
        "flowchart TD\n  classDef bad fill:#EAECEF,stroke:#7D8590;",
        "without text color",
    )
    require_issue(
        "hardcoded white",
        "flowchart TD\n  classDef bad color:#fff,stroke:#7D8590;",
        "hardcodes black or white",
    )
    require_issue(
        "low contrast",
        "flowchart TD\n  classDef bad fill:#777,color:#888,stroke:#555;",
        "contrast",
    )

    safe = (
        "flowchart TD\n"
        "  A[\"Allowed\"]:::success\n"
        "  classDef success fill:#E6F4EA,stroke:#1E7E45,color:#1A1A1A;"
    )
    errors = check_mermaid_block(safe, 1)
    if errors:
        raise AssertionError(f"safe palette was rejected: {errors!r}")

    print("test_readme_quality_unit: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
