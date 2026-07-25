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
import time
import zipfile


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")
ZIP_EPOCH = 315532800


def parse_args():
    parser = argparse.ArgumentParser(
        description="Create deterministic rl-c-client source archives"
    )
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    return parser.parse_args()


def read_allowlist(source):
    allowlist = source / "manifest" / "source-release.files"
    paths = []
    for raw_line in allowlist.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        relative = PurePosixPath(line)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"unsafe source-release path: {line}")
        if relative.as_posix() in paths:
            raise ValueError(f"duplicate source-release path: {line}")
        candidate = source.joinpath(*relative.parts)
        if not candidate.is_file() or candidate.is_symlink():
            raise ValueError(f"source-release path is not a regular file: {line}")
        paths.append(relative.as_posix())
    if paths != sorted(paths):
        raise ValueError("manifest/source-release.files must be sorted")
    return paths


def sha256(content):
    return hashlib.sha256(content).hexdigest()


def collect_files(source, version, commit, epoch):
    files = {}
    manifest_entries = []
    for relative in read_allowlist(source):
        content = (source / relative).read_bytes()
        files[relative] = content
        manifest_entries.append(
            {
                "path": relative,
                "sha256": sha256(content),
                "size": len(content),
            }
        )

    version_content = f"{version}\n".encode()
    files["VERSION"] = version_content
    manifest_entries.append(
        {
            "path": "VERSION",
            "sha256": sha256(version_content),
            "size": len(version_content),
        }
    )
    manifest_entries.sort(key=lambda item: item["path"])
    manifest = {
        "commit": commit,
        "files": manifest_entries,
        "source_date_epoch": epoch,
        "version": version,
    }
    files["SOURCE-MANIFEST.json"] = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode()
    return files


def write_tar_gz(path, root_name, files, epoch):
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
                for relative, content in sorted(files.items()):
                    info = tarfile.TarInfo(f"{root_name}/{relative}")
                    info.size = len(content)
                    info.mtime = epoch
                    info.mode = 0o644
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    archive.addfile(info, io.BytesIO(content))


def write_zip(path, root_name, files, epoch):
    zip_timestamp = time.gmtime(max(epoch, ZIP_EPOCH))[:6]
    with zipfile.ZipFile(
        path,
        mode="w",
        compression=zipfile.ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for relative, content in sorted(files.items()):
            info = zipfile.ZipInfo(
                filename=f"{root_name}/{relative}",
                date_time=zip_timestamp,
            )
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, content, compresslevel=9)


def main():
    args = parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if not SEMVER.fullmatch(args.version):
        raise ValueError("--version must be numeric MAJOR.MINOR.PATCH")
    if not COMMIT.fullmatch(args.commit):
        raise ValueError("--commit must be a lowercase 40-character SHA")
    epoch_value = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_value is None or not epoch_value.isdigit():
        raise ValueError("SOURCE_DATE_EPOCH must be a non-negative integer")
    epoch = int(epoch_value)
    output.mkdir(parents=True, exist_ok=True)

    files = collect_files(source, args.version, args.commit, epoch)
    root_name = f"rl-c-client-{args.version}"
    basename = f"rl-c-client-v{args.version}-source"
    write_tar_gz(output / f"{basename}.tar.gz", root_name, files, epoch)
    write_zip(output / f"{basename}.zip", root_name, files, epoch)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"package_source: {error}", file=sys.stderr)
        sys.exit(1)
