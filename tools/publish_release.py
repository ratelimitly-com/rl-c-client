#!/usr/bin/env python3
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import urlencode, urlsplit


class ReleaseError(RuntimeError):
    pass


class NotFound(ReleaseError):
    pass


class GitHub:
    def request(
        self,
        path,
        *,
        method="GET",
        fields=None,
        raw_fields=None,
        paginate=False,
    ):
        command = ["gh", "api", path, "--method", method]
        if paginate:
            command.extend(["--paginate", "--slurp"])
        for key, value in sorted((fields or {}).items()):
            command.extend(["-f", f"{key}={value}"])
        for key, value in sorted((raw_fields or {}).items()):
            if isinstance(value, bool):
                value = str(value).lower()
            command.extend(["-F", f"{key}={value}"])
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            message = result.stderr.strip()
            if "HTTP 404" in message or "Not Found" in message:
                raise NotFound(message)
            raise ReleaseError(message or f"gh api failed: {path}")
        output = result.stdout.strip()
        if not output:
            return None
        try:
            return json.loads(output)
        except json.JSONDecodeError as error:
            raise ReleaseError(f"invalid GitHub response for {path}") from error

    def upload(self, upload_url, name, input_path):
        template = "{?name,label}"
        if not isinstance(upload_url, str) or not upload_url.endswith(template):
            raise ReleaseError("release has invalid upload URL template")
        parsed = urlsplit(upload_url.removesuffix(template))
        if (
            parsed.scheme != "https"
            or parsed.hostname != "uploads.github.com"
            or parsed.port is not None
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
            or re.fullmatch(
                r"/repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/"
                r"releases/[0-9]+/assets",
                parsed.path,
            )
            is None
        ):
            raise ReleaseError("release has invalid upload URL")
        token = os.environ.get("GH_TOKEN")
        if not token:
            raise ReleaseError("GH_TOKEN is required")
        payload = Path(input_path).read_bytes()
        target = f"{parsed.path}?{urlencode({'name': name})}"
        connection = http.client.HTTPSConnection(
            "uploads.github.com",
            timeout=60,
        )
        try:
            connection.request(
                "POST",
                target,
                body=payload,
                headers={
                    "Accept": "application/vnd.github+json",
                    "Authorization": f"Bearer {token}",
                    "Content-Type": "application/octet-stream",
                    "User-Agent": "rl-c-client-release",
                    "X-GitHub-Api-Version": "2022-11-28",
                },
            )
            response = connection.getresponse()
            response.read()
            if response.status != 201:
                raise ReleaseError(
                    f"release asset upload returned HTTP {response.status}"
                )
        finally:
            connection.close()


def _flatten_pages(response):
    if not isinstance(response, list):
        raise ReleaseError("paginated GitHub response is not a list")
    if response and all(isinstance(page, list) for page in response):
        return [item for page in response for item in page]
    return response


def _resolve_tag_commit(github, repository, tag):
    try:
        reference = github.request(
            f"repos/{repository}/git/ref/tags/{tag}"
        )
    except NotFound:
        return None
    target = reference.get("object", {})
    for _ in range(8):
        object_type = target.get("type")
        sha = target.get("sha")
        if not isinstance(sha, str):
            raise ReleaseError(f"{tag} has an invalid Git object")
        if object_type == "commit":
            return sha
        if object_type != "tag":
            raise ReleaseError(f"{tag} points to unsupported {object_type}")
        annotated_tag = github.request(
            f"repos/{repository}/git/tags/{sha}"
        )
        target = annotated_tag.get("object", {})
    raise ReleaseError(f"{tag} has excessive annotated-tag depth")


def _release_assets(github, repository, release_id):
    response = github.request(
        f"repos/{repository}/releases/{release_id}/assets?per_page=100",
        paginate=True,
    )
    return _flatten_pages(response)


def _digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return f"sha256:{hasher.hexdigest()}"


def publish(
    *,
    github,
    repository,
    tag,
    version,
    commit,
    assets_directory,
):
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise ReleaseError("repository must be OWNER/NAME")
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise ReleaseError("version must be numeric MAJOR.MINOR.PATCH")
    if tag != f"v{version}":
        raise ReleaseError(f"tag {tag} does not match version {version}")
    if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise ReleaseError("commit must be a full lowercase SHA-1")

    assets_directory = Path(assets_directory)
    if not assets_directory.is_dir():
        raise ReleaseError("release asset directory does not exist")
    assets = sorted(
        path for path in assets_directory.iterdir()
        if path.is_file() and not path.is_symlink()
    )
    if not assets:
        raise ReleaseError("release asset directory is empty")
    asset_names = {path.name for path in assets}
    for required in ("RELEASE-MANIFEST.json", "SHA256SUMS"):
        if required not in asset_names:
            raise ReleaseError(f"release assets lack {required}")

    tag_commit = _resolve_tag_commit(github, repository, tag)
    if tag_commit is not None and tag_commit != commit:
        raise ReleaseError(
            f"{tag} points to {tag_commit}, expected {commit}"
        )

    releases = _flatten_pages(
        github.request(
            f"repos/{repository}/releases?per_page=100",
            paginate=True,
        )
    )
    matches = [
        release for release in releases
        if release.get("tag_name") == tag
    ]
    if len(matches) > 1:
        raise ReleaseError(f"multiple releases found for {tag}")
    if matches:
        release = matches[0]
        if release.get("draft") is not True:
            raise ReleaseError(f"refusing to overwrite published release {tag}")
        if release.get("target_commitish") != commit:
            raise ReleaseError(
                f"draft {tag} targets {release.get('target_commitish')}, "
                f"expected {commit}"
            )
    else:
        release = None

    if tag_commit is None:
        github.request(
            f"repos/{repository}/git/refs",
            method="POST",
            fields={
                "ref": f"refs/tags/{tag}",
                "sha": commit,
            },
        )

    if release is None:
        release = github.request(
            f"repos/{repository}/releases",
            method="POST",
            fields={
                "name": tag,
                "tag_name": tag,
                "target_commitish": commit,
            },
            raw_fields={
                "draft": True,
                "generate_release_notes": True,
            },
        )

    release_id = release.get("id")
    if not isinstance(release_id, int):
        raise ReleaseError(f"release {tag} has no numeric ID")
    upload_url = release.get("upload_url")
    if not isinstance(upload_url, str):
        raise ReleaseError(f"release {tag} has no upload URL")

    remote_assets = _release_assets(github, repository, release_id)
    remote_by_name = {}
    for asset in remote_assets:
        name = asset.get("name")
        asset_id = asset.get("id")
        if not isinstance(name, str) or not isinstance(asset_id, int):
            raise ReleaseError(f"release {tag} has invalid asset metadata")
        if name in remote_by_name:
            raise ReleaseError(f"release {tag} has duplicate asset {name}")
        remote_by_name[name] = asset
    unexpected = sorted(set(remote_by_name) - asset_names)
    if unexpected:
        raise ReleaseError(
            f"release {tag} has unexpected assets: {', '.join(unexpected)}"
        )

    for asset in assets:
        previous = remote_by_name.get(asset.name)
        if previous is not None:
            github.request(
                f"repos/{repository}/releases/assets/{previous['id']}",
                method="DELETE",
            )
        github.upload(upload_url, asset.name, asset)

    actual_assets = _release_assets(github, repository, release_id)
    actual = {}
    for asset in actual_assets:
        name = asset.get("name")
        digest = asset.get("digest")
        if not isinstance(name, str) or not isinstance(digest, str):
            raise ReleaseError(f"release {tag} lacks remote asset digests")
        if name in actual:
            raise ReleaseError(f"release {tag} has duplicate asset {name}")
        actual[name] = digest
    expected = {asset.name: _digest(asset) for asset in assets}
    if actual != expected:
        raise ReleaseError(
            f"release {tag} remote asset names or digests do not match"
        )

    published = github.request(
        f"repos/{repository}/releases/{release_id}",
        method="PATCH",
        raw_fields={"draft": False},
    )
    url = published.get("html_url")
    if not isinstance(url, str) or not url:
        raise ReleaseError(f"published release {tag} has no URL")
    return url


def parse_args():
    parser = argparse.ArgumentParser(
        description="Resume or create, verify, and publish a GitHub release."
    )
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    try:
        url = publish(
            github=GitHub(),
            repository=args.repository,
            tag=args.tag,
            version=args.version,
            commit=args.commit,
            assets_directory=args.assets,
        )
    except (OSError, ReleaseError) as error:
        print(f"publish_release: {error}", file=sys.stderr)
        return 1
    print(url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
