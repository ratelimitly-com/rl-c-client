#!/usr/bin/env python3
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "publish_release.py"
SPEC = importlib.util.spec_from_file_location("publish_release", MODULE_PATH)
publish_release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish_release)


class FakeGitHub:
    def __init__(self, commit):
        self.commit = commit
        self.refs = {}
        self.releases = []
        self.assets = {}
        self.events = []
        self.next_release_id = 100
        self.next_asset_id = 1000

    def request(
        self,
        path,
        *,
        method="GET",
        fields=None,
        raw_fields=None,
        hostname=None,
        input_path=None,
        paginate=False,
    ):
        fields = fields or {}
        raw_fields = raw_fields or {}
        self.events.append((method, hostname, path))

        if path.endswith("/git/ref/tags/v0.3.0") and method == "GET":
            if "v0.3.0" not in self.refs:
                raise publish_release.NotFound("missing tag")
            return {
                "object": {
                    "type": "commit",
                    "sha": self.refs["v0.3.0"],
                }
            }
        if path.endswith("/git/refs") and method == "POST":
            tag = fields["ref"].removeprefix("refs/tags/")
            self.refs[tag] = fields["sha"]
            return {"ref": fields["ref"]}
        if path.endswith("/releases?per_page=100") and method == "GET":
            return [dict(release) for release in self.releases]
        if path.endswith("/releases") and method == "POST":
            release = {
                "id": self.next_release_id,
                "tag_name": fields["tag_name"],
                "name": fields["name"],
                "target_commitish": fields["target_commitish"],
                "draft": raw_fields["draft"],
                "html_url": "https://example.invalid/draft",
            }
            self.next_release_id += 1
            self.releases.append(release)
            self.assets[release["id"]] = []
            return dict(release)
        if "/releases/assets/" in path and method == "DELETE":
            asset_id = int(path.rsplit("/", 1)[1])
            for release_assets in self.assets.values():
                release_assets[:] = [
                    asset for asset in release_assets
                    if asset["id"] != asset_id
                ]
            return None
        if path.endswith("/assets?per_page=100") and method == "GET":
            release_id = int(path.split("/releases/", 1)[1].split("/", 1)[0])
            return [dict(asset) for asset in self.assets[release_id]]
        if path.endswith("/assets") and method == "POST":
            if hostname != "uploads.github.com":
                raise AssertionError("release assets must use upload API")
            release_id = int(path.split("/releases/", 1)[1].split("/", 1)[0])
            payload = Path(input_path).read_bytes()
            asset = {
                "id": self.next_asset_id,
                "name": fields["name"],
                "digest": f"sha256:{hashlib.sha256(payload).hexdigest()}",
            }
            self.next_asset_id += 1
            self.assets[release_id].append(asset)
            return dict(asset)
        if "/releases/" in path and method == "PATCH":
            release_id = int(path.rsplit("/", 1)[1])
            release = next(
                release for release in self.releases
                if release["id"] == release_id
            )
            release["draft"] = raw_fields["draft"]
            release["html_url"] = (
                "https://example.invalid/releases/tag/v0.3.0"
            )
            return dict(release)
        raise AssertionError(f"unexpected request: {method} {hostname} {path}")


class PublishReleaseTests(unittest.TestCase):
    COMMIT = "3" * 40

    def make_assets(self, root):
        assets = root / "release-assets"
        assets.mkdir()
        (assets / "RELEASE-MANIFEST.json").write_text(
            '{"version":"0.3.0"}\n',
            encoding="utf-8",
        )
        (assets / "SHA256SUMS").write_text(
            "placeholder\n",
            encoding="utf-8",
        )
        (assets / "rl-c-client-v0.3.0-source.tar.gz").write_bytes(
            b"source archive"
        )
        return assets

    def test_resumes_untagged_draft_by_id_and_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = self.make_assets(Path(directory))
            github = FakeGitHub(self.COMMIT)
            github.releases.append(
                {
                    "id": 7,
                    "tag_name": "v0.3.0",
                    "name": "v0.3.0",
                    "target_commitish": self.COMMIT,
                    "draft": True,
                    "html_url": "https://example.invalid/untagged-draft",
                }
            )
            github.assets[7] = [
                {
                    "id": 55,
                    "name": "SHA256SUMS",
                    "digest": "sha256:stale",
                }
            ]

            result = publish_release.publish(
                github=github,
                repository="acme/repo",
                tag="v0.3.0",
                version="0.3.0",
                commit=self.COMMIT,
                assets_directory=assets,
            )

            self.assertEqual(github.refs["v0.3.0"], self.COMMIT)
            self.assertEqual(len(github.releases), 1)
            self.assertFalse(github.releases[0]["draft"])
            self.assertEqual(
                {asset["name"] for asset in github.assets[7]},
                {path.name for path in assets.iterdir()},
            )
            self.assertEqual(
                result,
                "https://example.invalid/releases/tag/v0.3.0",
            )
            create_ref = github.events.index(
                ("POST", None, "repos/acme/repo/git/refs")
            )
            first_upload = next(
                index for index, event in enumerate(github.events)
                if event[0] == "POST"
                and event[1] == "uploads.github.com"
            )
            self.assertLess(create_ref, first_upload)
            self.assertIn(
                (
                    "DELETE",
                    None,
                    "repos/acme/repo/releases/assets/55",
                ),
                github.events,
            )

    def test_refuses_tag_that_points_to_another_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = self.make_assets(Path(directory))
            github = FakeGitHub(self.COMMIT)
            github.refs["v0.3.0"] = "4" * 40

            with self.assertRaisesRegex(
                publish_release.ReleaseError,
                "points to",
            ):
                publish_release.publish(
                    github=github,
                    repository="acme/repo",
                    tag="v0.3.0",
                    version="0.3.0",
                    commit=self.COMMIT,
                    assets_directory=assets,
                )

            self.assertEqual(github.releases, [])

    def test_refuses_draft_for_another_commit_without_creating_tag(self):
        with tempfile.TemporaryDirectory() as directory:
            assets = self.make_assets(Path(directory))
            github = FakeGitHub(self.COMMIT)
            github.releases.append(
                {
                    "id": 7,
                    "tag_name": "v0.3.0",
                    "name": "v0.3.0",
                    "target_commitish": "4" * 40,
                    "draft": True,
                    "html_url": "https://example.invalid/untagged-draft",
                }
            )
            github.assets[7] = []

            with self.assertRaisesRegex(
                publish_release.ReleaseError,
                "draft v0.3.0 targets",
            ):
                publish_release.publish(
                    github=github,
                    repository="acme/repo",
                    tag="v0.3.0",
                    version="0.3.0",
                    commit=self.COMMIT,
                    assets_directory=assets,
                )

            self.assertEqual(github.refs, {})


if __name__ == "__main__":
    unittest.main()
