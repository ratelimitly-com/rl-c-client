#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
VERSION = "1.2.3"
COMMIT = "0123456789abcdef0123456789abcdef01234567"
EPOCH = "1700000000"


def sha256(path):
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def build(output):
    env = os.environ.copy()
    env["SOURCE_DATE_EPOCH"] = EPOCH
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "tools" / "package_source.py"),
            "--source",
            str(ROOT),
            "--output",
            str(output),
            "--version",
            VERSION,
            "--commit",
            COMMIT,
        ],
        check=True,
        env=env,
    )


def verify_members(names):
    prefix = f"rl-c-client-{VERSION}/"
    assert all(name.startswith(prefix) for name in names)
    relative = {name.removeprefix(prefix) for name in names}
    assert "VERSION" in relative
    assert "SOURCE-MANIFEST.json" in relative
    assert "EMBEDDING.md" in relative
    assert "cmake/rclient-embed.cmake" in relative
    assert "include/r_client.h" in relative
    assert "src/r_client.c" in relative
    assert "tests/test_protocol.c" not in relative
    assert not any(name.startswith(".git") for name in relative)
    assert not any(name.startswith(".github") for name in relative)
    assert not any(name.startswith("examples/") for name in relative)
    return relative


def main():
    with tempfile.TemporaryDirectory(prefix="rl-source-release-") as tmp:
        tmp_path = Path(tmp)
        first = tmp_path / "first"
        second = tmp_path / "second"
        first.mkdir()
        second.mkdir()
        build(first)
        build(second)

        archive_names = [
            f"rl-c-client-v{VERSION}-source.tar.gz",
            f"rl-c-client-v{VERSION}-source.zip",
        ]
        for archive_name in archive_names:
            assert sha256(first / archive_name) == sha256(second / archive_name)

        with tarfile.open(first / archive_names[0], "r:gz") as archive:
            tar_files = {
                member.name
                for member in archive.getmembers()
                if member.isfile()
            }
            verify_members(tar_files)
            prefix = f"rl-c-client-{VERSION}/"
            manifest_file = archive.extractfile(
                prefix + "SOURCE-MANIFEST.json"
            )
            assert manifest_file is not None
            manifest = json.load(manifest_file)

        with zipfile.ZipFile(first / archive_names[1]) as archive:
            zip_files = {
                info.filename
                for info in archive.infolist()
                if not info.is_dir()
            }
            verify_members(zip_files)

        assert tar_files == zip_files
        assert manifest["version"] == VERSION
        assert manifest["commit"] == COMMIT
        assert manifest["source_date_epoch"] == int(EPOCH)
        assert manifest["files"] == sorted(
            manifest["files"], key=lambda item: item["path"]
        )
        assert all(
            len(item["sha256"]) == 64 and item["size"] >= 0
            for item in manifest["files"]
        )

    print("test_source_release: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
