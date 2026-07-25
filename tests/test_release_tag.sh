#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/rl-release-tag.XXXXXX")"
trap 'rm -rf "${work}"' EXIT

export GNUPGHOME="${work}/signer"
install -d -m 0700 "${GNUPGHOME}"
gpg --batch --passphrase "" --quick-gen-key \
    "Release Test <release-test@example.invalid>" ed25519 sign 0
fingerprint="$(
    gpg --batch --with-colons --fingerprint |
        awk -F: '$1 == "fpr" {print $10; exit}'
)"
gpg --batch --armor --export "${fingerprint}" >"${work}/allowed.asc"

git init -q -b main "${work}/repo"
git -C "${work}/repo" config user.name "Release Test"
git -C "${work}/repo" config user.email "release-test@example.invalid"
git -C "${work}/repo" config user.signingkey "${fingerprint}"
git -C "${work}/repo" config gpg.program gpg
printf 'main\n' >"${work}/repo/state"
git -C "${work}/repo" add state
git -C "${work}/repo" commit -qm "main"
main_commit="$(git -C "${work}/repo" rev-parse HEAD)"
git -C "${work}/repo" tag -s v1.2.3 -m "v1.2.3"

(
    cd "${work}/repo"
    bash "${root}/tools/verify_release_tag.sh" \
        v1.2.3 refs/heads/main "${main_commit}" "${work}/allowed.asc"
)

git -C "${work}/repo" tag v1.2.4
if (
    cd "${work}/repo"
    bash "${root}/tools/verify_release_tag.sh" \
        v1.2.4 refs/heads/main "${main_commit}" "${work}/allowed.asc"
); then
    echo "lightweight release tag was accepted" >&2
    exit 1
fi

git -C "${work}/repo" switch -qc side
printf 'side\n' >>"${work}/repo/state"
git -C "${work}/repo" commit -qam "side"
side_commit="$(git -C "${work}/repo" rev-parse HEAD)"
git -C "${work}/repo" tag -s v2.0.0 -m "v2.0.0"
if (
    cd "${work}/repo"
    bash "${root}/tools/verify_release_tag.sh" \
        v2.0.0 refs/heads/main "${side_commit}" "${work}/allowed.asc"
); then
    echo "off-main release tag was accepted" >&2
    exit 1
fi

echo "test_release_tag: PASS"
