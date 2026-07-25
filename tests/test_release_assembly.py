#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.2.3"
COMMIT = "0123456789abcdef0123456789abcdef01234567"


def expected_names():
    config = json.loads(
        (ROOT / "manifest" / "release-assets.json").read_text(
            encoding="utf-8"
        )
    )
    return sorted(
        artifact["name"].format(version=VERSION)
        for artifact in config["artifacts"]
    )


def assemble(input_dir, output_dir, expect_success=True):
    env = os.environ.copy()
    env["SOURCE_DATE_EPOCH"] = "1700000000"
    result = subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "assemble_release.py"),
            "--input",
            str(input_dir),
            "--output",
            str(output_dir),
            "--version",
            VERSION,
            "--commit",
            COMMIT,
        ],
        env=env,
        text=True,
        capture_output=True,
    )
    if expect_success and result.returncode != 0:
        raise AssertionError(result.stderr)
    if not expect_success and result.returncode == 0:
        raise AssertionError("assembly unexpectedly succeeded")
    return result


def main():
    names = expected_names()
    assert len(names) == 19
    assert names == sorted(set(names))

    with tempfile.TemporaryDirectory(prefix="rl-release-assembly-") as tmp:
        tmp_path = Path(tmp)
        inputs = tmp_path / "inputs"
        first = tmp_path / "first"
        second = tmp_path / "second"
        inputs.mkdir()
        for name in names:
            (inputs / name).write_bytes(f"fixture:{name}\n".encode())

        assemble(inputs, first)
        assemble(inputs, second)
        output_names = sorted(path.name for path in first.iterdir())
        assert output_names == sorted(
            names + ["RELEASE-MANIFEST.json", "SHA256SUMS"]
        )
        for name in output_names:
            assert (first / name).read_bytes() == (second / name).read_bytes()

        manifest = json.loads(
            (first / "RELEASE-MANIFEST.json").read_text(encoding="utf-8")
        )
        assert manifest["version"] == VERSION
        assert manifest["commit"] == COMMIT
        assert manifest["source_date_epoch"] == 1700000000
        assert [entry["name"] for entry in manifest["artifacts"]] == names
        checksum_lines = (
            first / "SHA256SUMS"
        ).read_text(encoding="utf-8").splitlines()
        assert len(checksum_lines) == len(names)
        for line, name in zip(checksum_lines, names, strict=True):
            digest, listed_name = line.split("  ", maxsplit=1)
            assert listed_name == name
            assert digest == hashlib.sha256((first / name).read_bytes()).hexdigest()

        missing_inputs = tmp_path / "missing"
        missing_inputs.mkdir()
        for name in names[1:]:
            (missing_inputs / name).write_bytes(b"fixture\n")
        result = assemble(
            missing_inputs, tmp_path / "missing-output", expect_success=False
        )
        assert "missing release artifacts" in result.stderr

        (inputs / "unexpected.txt").write_bytes(b"unexpected\n")
        result = assemble(
            inputs, tmp_path / "unexpected-output", expect_success=False
        )
        assert "unexpected release artifacts" in result.stderr

    print("test_release_assembly: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
