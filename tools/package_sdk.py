#!/usr/bin/env python3
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import tarfile


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
ARCHITECTURES = {"amd64", "aarch64", "universal2"}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Create a deterministic rl-c-client SDK archive"
    )
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--architecture", required=True)
    return parser.parse_args()


def sha256(content):
    return hashlib.sha256(content).hexdigest()


def collect_entries(stage, version, commit, platform, architecture, epoch):
    entries = {}
    manifest_entries = []
    for path in sorted(stage.rglob("*")):
        if path.is_dir():
            continue
        relative = path.relative_to(stage).as_posix()
        if relative == "SDK-MANIFEST.json":
            continue
        safe_relative = PurePosixPath(relative)
        if safe_relative.is_absolute() or ".." in safe_relative.parts:
            raise ValueError(f"unsafe SDK path: {relative}")
        if path.is_symlink():
            target = os.readlink(path)
            if "/" in target or target in {"", ".", ".."}:
                raise ValueError(f"unsafe SDK symlink: {relative} -> {target}")
            entries[relative] = ("symlink", target)
            manifest_entries.append(
                {
                    "path": relative,
                    "target": target,
                    "type": "symlink",
                }
            )
        elif path.is_file():
            content = path.read_bytes()
            entries[relative] = ("file", content)
            manifest_entries.append(
                {
                    "path": relative,
                    "sha256": sha256(content),
                    "size": len(content),
                    "type": "file",
                }
            )
        else:
            raise ValueError(f"unsupported SDK entry: {relative}")

    if not entries:
        raise ValueError("SDK staging directory is empty")
    manifest = {
        "architecture": architecture,
        "commit": commit,
        "files": manifest_entries,
        "platform": platform,
        "source_date_epoch": epoch,
        "version": version,
    }
    manifest_content = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode()
    entries["SDK-MANIFEST.json"] = ("file", manifest_content)
    return entries


def write_tar_gz(path, root_name, entries, epoch):
    with path.open("wb") as raw_file:
        with gzip.GzipFile(
            filename="",
            mode="wb",
            fileobj=raw_file,
            compresslevel=9,
            mtime=epoch,
        ) as gzip_file:
            with tarfile.open(
                fileobj=gzip_file,
                mode="w",
                format=tarfile.GNU_FORMAT,
            ) as archive:
                for relative, entry in sorted(entries.items()):
                    entry_type, payload = entry
                    info = tarfile.TarInfo(f"{root_name}/{relative}")
                    info.mtime = epoch
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    if entry_type == "symlink":
                        info.type = tarfile.SYMTYPE
                        info.mode = 0o777
                        info.linkname = payload
                        archive.addfile(info)
                    else:
                        info.size = len(payload)
                        info.mode = 0o644
                        archive.addfile(info, io.BytesIO(payload))


def main():
    args = parse_args()
    stage = args.stage.resolve()
    output = args.output.resolve()
    if not stage.is_dir():
        raise ValueError("--stage must be a directory")
    if not SEMVER.fullmatch(args.version):
        raise ValueError("--version must be numeric MAJOR.MINOR.PATCH")
    if not COMMIT.fullmatch(args.commit):
        raise ValueError("--commit must be a lowercase 40-character SHA")
    if args.architecture not in ARCHITECTURES:
        raise ValueError("unsupported --architecture")
    if not re.fullmatch(r"[a-z0-9.-]+", args.platform):
        raise ValueError("unsupported --platform")
    epoch_value = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_value is None or not epoch_value.isdigit():
        raise ValueError("SOURCE_DATE_EPOCH must be a non-negative integer")
    epoch = int(epoch_value)

    output.mkdir(parents=True, exist_ok=True)
    entries = collect_entries(
        stage,
        args.version,
        args.commit,
        args.platform,
        args.architecture,
        epoch,
    )
    root_name = (
        f"rl-c-client-{args.version}-{args.platform}-{args.architecture}"
    )
    archive_name = (
        f"rl-c-client-v{args.version}-{args.platform}-"
        f"{args.architecture}-sdk.tar.gz"
    )
    write_tar_gz(output / archive_name, root_name, entries, epoch)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"package_sdk: {error}", file=sys.stderr)
        sys.exit(1)
