#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main():
    script = (ROOT / "tests" / "test_windows_native_example.ps1").read_text(
        encoding="utf-8"
    )

    assert "$ExpectedRateRequestCountMax = 2" in script
    assert "$RateRecords.Count -lt 1 -or" in script
    assert "$RateRecords.Count -gt $ExpectedRateRequestCountMax" in script
    assert "foreach ($Rate in $RateRecords)" in script
    assert "$Rate = $RateRecords[0]" not in script
    for name in (
        "RATELIMITLY_REQUEST_UNIT_MS",
        "RATELIMITLY_REQUEST_REPLAY_COUNT",
        "RATELIMITLY_REQUEST_PROFILE",
    ):
        assert f'$env:{name} = $null' in script
        assert f'"{name}"' in script

    print("test_windows_native_example_contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
