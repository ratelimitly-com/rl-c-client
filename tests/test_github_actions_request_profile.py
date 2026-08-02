#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXPECTED_SETTINGS = {
    "RATELIMITLY_REQUEST_UNIT_MS": "25",
    "RATELIMITLY_REQUEST_REPLAY_COUNT": "3",
    "RATELIMITLY_REQUEST_PROFILE": "1",
}


def top_level_env(workflow: Path) -> dict[str, str]:
    lines = workflow.read_text(encoding="utf-8").splitlines()
    try:
        start = lines.index("env:") + 1
    except ValueError as error:
        raise AssertionError(f"{workflow.name} has no top-level env block") from error

    result: dict[str, str] = {}
    for line in lines[start:]:
        if line and not line.startswith(" "):
            break
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        name, separator, value = stripped.partition(":")
        assert separator, f"invalid env entry in {workflow.name}: {line}"
        result[name] = value.strip().strip('"\'')
    return result


def main() -> int:
    workflow_text: dict[str, str] = {}
    for relative in (
        ".github/workflows/ci.yml",
        ".github/workflows/release.yml",
    ):
        workflow = ROOT / relative
        workflow_text[relative] = workflow.read_text(encoding="utf-8")
        actual = top_level_env(workflow)
        for name, expected in EXPECTED_SETTINGS.items():
            assert actual.get(name) == expected, (
                f"{relative} sets {name}={actual.get(name)!r}; "
                f"expected {expected!r}"
            )

    release = workflow_text[".github/workflows/release.yml"]
    for name in EXPECTED_SETTINGS:
        assert release.count(f"--env {name}") >= 2, (
            f"release container test boundaries do not pass {name} through"
        )

    native = (ROOT / "tests/test_windows_native_example.ps1").read_text(
        encoding="utf-8"
    )
    for name, expected in EXPECTED_SETTINGS.items():
        assert f'$env:{name} = "{expected}"' in native
        assert f"$env:{name} = $null" not in native
    assert "$ExpectedRateRequestCountMax = 4" in native

    print("test_github_actions_request_profile: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
