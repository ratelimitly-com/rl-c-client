#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tarfile
import zipfile


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")


def read_archive(path):
    files = {}
    if path.name.endswith(".tar.gz"):
        with tarfile.open(path, "r:gz") as archive:
            members = archive.getmembers()
            if any(not member.isfile() for member in members):
                raise ValueError("source tar contains a non-regular entry")
            for member in members:
                if member.name in files:
                    raise ValueError(f"duplicate archive path: {member.name}")
                extracted = archive.extractfile(member)
                if extracted is None:
                    raise ValueError(f"cannot read archive path: {member.name}")
                files[member.name] = extracted.read()
    elif path.suffix == ".zip":
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if info.is_dir():
                    raise ValueError("source ZIP contains a directory entry")
                if info.filename in files:
                    raise ValueError(
                        f"duplicate archive path: {info.filename}"
                    )
                files[info.filename] = archive.read(info)
    else:
        raise ValueError("source archive must end in .tar.gz or .zip")
    return files


def verify(path):
    files = read_archive(path)
    if not files:
        raise ValueError("source archive is empty")

    roots = set()
    relative_files = {}
    for name, content in files.items():
        archive_path = PurePosixPath(name)
        if (
            archive_path.is_absolute()
            or ".." in archive_path.parts
            or len(archive_path.parts) < 2
        ):
            raise ValueError(f"unsafe source archive path: {name}")
        roots.add(archive_path.parts[0])
        relative = PurePosixPath(*archive_path.parts[1:]).as_posix()
        if relative in relative_files:
            raise ValueError(f"duplicate relative source path: {relative}")
        relative_files[relative] = content
    if len(roots) != 1:
        raise ValueError("source archive must have exactly one root")
    root = roots.pop()
    prefix = "rl-c-client-"
    if not root.startswith(prefix):
        raise ValueError("source archive root has an invalid name")
    root_version = root.removeprefix(prefix)
    if not SEMVER.fullmatch(root_version):
        raise ValueError("source archive root has an invalid version")

    try:
        manifest = json.loads(
            relative_files["SOURCE-MANIFEST.json"].decode("utf-8")
        )
    except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("source manifest is missing or invalid") from error
    if manifest.get("version") != root_version:
        raise ValueError("source manifest version does not match archive root")
    if not COMMIT.fullmatch(str(manifest.get("commit", ""))):
        raise ValueError("source manifest commit is invalid")
    if (
        not isinstance(manifest.get("source_date_epoch"), int)
        or manifest["source_date_epoch"] < 0
    ):
        raise ValueError("source manifest epoch is invalid")

    entries = manifest.get("files")
    if (
        not isinstance(entries, list)
        or not all(isinstance(item, dict) for item in entries)
        or entries != sorted(entries, key=lambda item: item.get("path", ""))
    ):
        raise ValueError("source manifest entries are not sorted")
    described = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {
            "path",
            "sha256",
            "size",
        }:
            raise ValueError("source manifest entry has an invalid shape")
        relative = entry["path"]
        relative_path = PurePosixPath(relative)
        if (
            not isinstance(relative, str)
            or relative_path.is_absolute()
            or ".." in relative_path.parts
            or relative in described
            or relative == "SOURCE-MANIFEST.json"
        ):
            raise ValueError(f"invalid source manifest path: {relative}")
        described[relative] = entry

    actual_paths = set(relative_files) - {"SOURCE-MANIFEST.json"}
    if set(described) != actual_paths:
        raise ValueError("source manifest paths do not match archive contents")
    for relative, entry in described.items():
        content = relative_files[relative]
        if entry["size"] != len(content):
            raise ValueError(f"source manifest size mismatch: {relative}")
        digest = hashlib.sha256(content).hexdigest()
        if entry["sha256"] != digest:
            raise ValueError(f"source manifest digest mismatch: {relative}")
    if relative_files.get("VERSION") != f"{root_version}\n".encode():
        raise ValueError("VERSION does not match the archive version")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <source-archive>", file=sys.stderr)
        return 2
    verify(Path(sys.argv[1]))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        OSError,
        tarfile.TarError,
        ValueError,
        zipfile.BadZipFile,
    ) as error:
        print(f"verify_source_archive: {error}", file=sys.stderr)
        sys.exit(1)
