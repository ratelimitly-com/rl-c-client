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
MERMAID_INIT_DIRECTIVE = re.compile(
    r"%%\{\s*(?:init|initialize)\b(.*?)\}%%", re.IGNORECASE | re.DOTALL
)
MERMAID_FORCED_KEY = re.compile(
    r"(?<![A-Za-z0-9_-])['\"]?(theme|background|lineColor)['\"]?\s*:",
    re.IGNORECASE,
)
MERMAID_STYLE_LINE = re.compile(
    r"^\s*(classDef|style|linkStyle)\b", re.IGNORECASE
)
MERMAID_HAS_FILL = re.compile(
    r"(?<![A-Za-z0-9_-])fill\s*:", re.IGNORECASE
)
MERMAID_HAS_COLOR = re.compile(
    r"(?<![A-Za-z0-9_-])color\s*:", re.IGNORECASE
)
MERMAID_FILL_HEX = re.compile(
    r"(?<![A-Za-z0-9_-])fill\s*:\s*#([0-9a-f]{3}|[0-9a-f]{6})\b",
    re.IGNORECASE,
)
MERMAID_COLOR_HEX = re.compile(
    r"(?<![A-Za-z0-9_-])color\s*:\s*#([0-9a-f]{3}|[0-9a-f]{6})\b",
    re.IGNORECASE,
)
MERMAID_PURE_BW_HEX = re.compile(
    r"#(?:000000|ffffff|000|fff)\b", re.IGNORECASE
)
MERMAID_PURE_BW_WORD = re.compile(
    r"(?<![A-Za-z])(?:black|white)(?![A-Za-z])", re.IGNORECASE
)
MIN_TEXT_CONTRAST = 4.5


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


def relative_luminance(hex_value: str) -> float:
    if len(hex_value) == 3:
        hex_value = "".join(character * 2 for character in hex_value)

    def channel(offset: int) -> float:
        value = int(hex_value[offset : offset + 2], 16) / 255.0
        if value <= 0.03928:
            return value / 12.92
        return ((value + 0.055) / 1.055) ** 2.4

    red, green, blue = (channel(offset) for offset in (0, 2, 4))
    return 0.2126 * red + 0.7152 * green + 0.0722 * blue


def contrast_ratio(first: str, second: str) -> float:
    first_luminance = relative_luminance(first)
    second_luminance = relative_luminance(second)
    lighter = max(first_luminance, second_luminance)
    darker = min(first_luminance, second_luminance)
    return (lighter + 0.05) / (darker + 0.05)


def check_mermaid_block(block: str, index: int) -> list[str]:
    errors: list[str] = []
    if not block.strip():
        return [f"Mermaid block {index} is empty"]

    for directive in MERMAID_INIT_DIRECTIVE.finditer(block):
        if MERMAID_FORCED_KEY.search(directive.group(1)):
            errors.append(f"Mermaid block {index} forces a theme color")

    lines = block.splitlines()
    if lines and lines[0].strip() == "---":
        frontmatter: list[str] = []
        for line in lines[1:]:
            if line.strip() == "---":
                break
            frontmatter.append(line)
        if MERMAID_FORCED_KEY.search("\n".join(frontmatter)):
            errors.append(f"Mermaid block {index} forces a theme color")

    for line_number, line in enumerate(lines, start=1):
        style = MERMAID_STYLE_LINE.search(line)
        if style and (
            MERMAID_PURE_BW_HEX.search(line)
            or MERMAID_PURE_BW_WORD.search(line)
        ):
            errors.append(
                f"Mermaid block {index}, line {line_number} hardcodes black or white"
            )

        if (
            style
            and style.group(1).lower() != "linkstyle"
            and MERMAID_HAS_FILL.search(line)
            and not MERMAID_HAS_COLOR.search(line)
        ):
            errors.append(
                f"Mermaid block {index}, line {line_number} sets fill without text color"
            )

        fill = MERMAID_FILL_HEX.search(line)
        color = MERMAID_COLOR_HEX.search(line)
        if style and fill and color:
            ratio = contrast_ratio(fill.group(1), color.group(1))
            if ratio < MIN_TEXT_CONTRAST:
                errors.append(
                    f"Mermaid block {index}, line {line_number} has "
                    f"{ratio:.2f}:1 fill/text contrast; expected at least "
                    f"{MIN_TEXT_CONTRAST:.1f}:1"
                )

    return errors


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
        errors.extend(check_mermaid_block(block, index))

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
