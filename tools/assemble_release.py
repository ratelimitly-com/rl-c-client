#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
ROOT = Path(__file__).resolve().parents[1]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Validate and assemble an rl-c-client release"
    )
    parser.add_argument("--input", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    return parser.parse_args()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_artifacts(version):
    config = json.loads(
        (ROOT / "manifest" / "release-assets.json").read_text(
            encoding="utf-8"
        )
    )
    artifacts = []
    names = set()
    for raw_artifact in config["artifacts"]:
        artifact = dict(raw_artifact)
        artifact["name"] = artifact["name"].format(version=version)
        if artifact["name"] in names:
            raise ValueError(
                f"duplicate expected release artifact: {artifact['name']}"
            )
        names.add(artifact["name"])
        artifacts.append(artifact)
    artifacts.sort(key=lambda item: item["name"])
    if [item["name"] for item in artifacts] != sorted(names):
        raise ValueError("release asset configuration is not deterministic")
    return artifacts


def discover_inputs(input_directories):
    discovered = {}
    for input_directory in input_directories:
        resolved = input_directory.resolve()
        if not resolved.is_dir():
            raise ValueError(f"release input is not a directory: {resolved}")
        for path in sorted(resolved.rglob("*")):
            if path.is_dir():
                continue
            if not path.is_file() or path.is_symlink():
                raise ValueError(f"release input is not a regular file: {path}")
            if path.name in discovered:
                raise ValueError(f"duplicate release artifact: {path.name}")
            discovered[path.name] = path
    return discovered


def main():
    args = parse_args()
    if not SEMVER.fullmatch(args.version):
        raise ValueError("--version must be numeric MAJOR.MINOR.PATCH")
    if not COMMIT.fullmatch(args.commit):
        raise ValueError("--commit must be a lowercase 40-character SHA")
    epoch_value = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_value is None or not epoch_value.isdigit():
        raise ValueError("SOURCE_DATE_EPOCH must be a non-negative integer")
    epoch = int(epoch_value)

    expected = expected_artifacts(args.version)
    expected_names = {item["name"] for item in expected}
    discovered = discover_inputs(args.input)
    discovered_names = set(discovered)
    missing = sorted(expected_names - discovered_names)
    unexpected = sorted(discovered_names - expected_names)
    if missing:
        raise ValueError(f"missing release artifacts: {', '.join(missing)}")
    if unexpected:
        raise ValueError(
            f"unexpected release artifacts: {', '.join(unexpected)}"
        )

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f"release output directory is not empty: {output}")

    manifest_artifacts = []
    checksum_lines = []
    for artifact in expected:
        name = artifact["name"]
        destination = output / name
        shutil.copyfile(discovered[name], destination)
        digest = sha256(destination)
        manifest_artifacts.append(
            {
                **artifact,
                "sha256": digest,
                "size": destination.stat().st_size,
            }
        )
        checksum_lines.append(f"{digest}  {name}\n")

    manifest = {
        "artifacts": manifest_artifacts,
        "commit": args.commit,
        "source_date_epoch": epoch,
        "version": args.version,
    }
    (output / "RELEASE-MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (output / "SHA256SUMS").write_text(
        "".join(checksum_lines),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"assemble_release: {error}", file=sys.stderr)
        sys.exit(1)
