#!/usr/bin/env python3
"""Check the teaching contract shared by every tracked README.

This is intentionally a small, dependency-free source gate.  It catches the
structural mistakes that are easy to reintroduce during maintenance; reviewers
still verify technical claims and read each document from top to bottom.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRST_HEADING_LIMIT = 20
FORWARD_REFERENCE = re.compile(
    r"\b(?:as (?:we(?:'ll| will)|you(?:'ll| will)) see|"
    r"see below|described later|in a later section)\b",
    re.IGNORECASE,
)


def tracked_readmes() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "README.md", "**/README.md"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [ROOT / name for name in sorted(result.stdout.splitlines())]


def section(text: str, heading: str) -> str:
    match = re.search(
        rf"(?ms)^{re.escape(heading)}\s*$\n(.*?)(?=^##\s|\Z)", text
    )
    return match.group(1).strip() if match else ""


def mermaid_blocks(text: str) -> list[str]:
    return re.findall(r"(?ms)^```mermaid\s*$\n(.*?)^```\s*$", text)


def glossary_rows(text: str) -> int:
    body = section(text, "## Glossary")
    rows = 0
    for line in body.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0].lower() not in {"term", "---", "----"}:
            if not all(re.fullmatch(r":?-+:?", cell) for cell in cells):
                rows += 1
    return rows


def check_readme(path: Path) -> list[str]:
    relative = path.relative_to(ROOT).as_posix()
    text = path.read_text(encoding="utf-8")
    errors: list[str] = []

    first_lines = "\n".join(text.splitlines()[:FIRST_HEADING_LIMIT])
    if not re.search(r"(?m)^> \*\*Prerequisites\.\*\* ", first_lines):
        errors.append("missing an explicit prerequisites contract near the top")

    h2s = re.findall(r"(?m)^## .+$", text)
    if not h2s or h2s[0] != "## TL;DR":
        errors.append("TL;DR is not the first level-two section")
    tldr = section(text, "## TL;DR")
    if not tldr:
        errors.append("missing TL;DR content")
    else:
        prose = re.sub(r"`[^`]+`|\[[^]]+\]\([^)]+\)", "term", tldr)
        sentence_ends = re.findall(r"[.!?](?=\s|$)", prose)
        if len(sentence_ends) > 2:
            errors.append(f"TL;DR exceeds two sentences ({len(sentence_ends)} found)")
        if not re.search(r"(?i)rate[- ]?limit", prose):
            errors.append("TL;DR does not state the rate-limiting role")
        if not re.search(r"(?i)latency", prose):
            errors.append("TL;DR does not state the latency-tracking role")

    blocks = mermaid_blocks(text)
    if not blocks:
        errors.append("missing a Mermaid overview")
    for index, block in enumerate(blocks, start=1):
        if re.search(r"(?i)%%\s*\{\s*(?:init|initialize)\b", block):
            errors.append(f"Mermaid block {index} forces initialization/theming")
        if re.search(r"(?i)\b(?:theme|background|lineColor)\s*:", block):
            errors.append(f"Mermaid block {index} forces a theme color")
        for line_number, line in enumerate(block.splitlines(), start=1):
            if re.search(r"(?i)\b(?:classDef|style)\b", line) and re.search(
                r"(?i)\bfill\s*:", line
            ):
                if not re.search(r"(?i)\bcolor\s*:", line):
                    errors.append(
                        f"Mermaid block {index}, line {line_number} sets fill without text color"
                    )
                if re.search(r"(?i)(?:color|fill)\s*:\s*(?:#(?:000|000000|fff|ffffff)\b|black\b|white\b)", line):
                    errors.append(
                        f"Mermaid block {index}, line {line_number} hardcodes black or white"
                    )

    if "## Glossary" not in text:
        errors.append("missing a glossary")
    elif glossary_rows(text) < 3:
        errors.append("glossary has fewer than three defined terms")

    if not re.search(r"(?m)^## (?:API references|References)$", text):
        errors.append("missing a references section")

    forward = FORWARD_REFERENCE.search(text)
    if forward:
        line = text.count("\n", 0, forward.start()) + 1
        errors.append(f"forward-reference phrase at line {line}: {forward.group(0)!r}")

    if relative.startswith("examples/") and relative != "examples/README.md":
        if "## What this example teaches" not in text:
            errors.append("missing 'What this example teaches'")
        if not re.search(r"\[[^]]+\]\((?:\./)?main\.c(?:#[^)]+)?\)", text):
            errors.append("missing a direct link to main.c")

    return errors


def main() -> int:
    readmes = tracked_readmes()
    if not readmes:
        print("test_readme_quality: no tracked README files", file=sys.stderr)
        return 1

    failures = 0
    for path in readmes:
        errors = check_readme(path)
        if not errors:
            continue
        failures += len(errors)
        relative = path.relative_to(ROOT)
        for error in errors:
            print(f"{relative}: {error}", file=sys.stderr)

    if failures:
        print(
            f"test_readme_quality: FAIL ({failures} issues across {len(readmes)} files)",
            file=sys.stderr,
        )
        return 1

    print(f"test_readme_quality: PASS ({len(readmes)} files)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
