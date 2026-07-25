#!/usr/bin/env bash
set -euo pipefail

if [[ "$#" -ne 4 ]]; then
    echo "usage: $0 <tag> <main-ref> <expected-commit> <allowed-keys>" >&2
    exit 2
fi

tag="$1"
main_ref="$2"
expected_commit="$3"
allowed_keys="$4"
keyring="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/rl-release-keys.XXXXXX")"
trap 'rm -rf "${keyring}"' EXIT

if [[ "$(git cat-file -t "refs/tags/${tag}")" != tag ]]; then
    echo "release tag must be annotated" >&2
    exit 1
fi
if [[ ! "${expected_commit}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "expected release commit must be a full SHA" >&2
    exit 2
fi
if [[ ! -f "${allowed_keys}" ]]; then
    echo "release signer keyring does not exist: ${allowed_keys}" >&2
    exit 2
fi

export GNUPGHOME="${keyring}"
chmod 0700 "${GNUPGHOME}"
gpg --batch --import "${allowed_keys}" >/dev/null
git verify-tag "${tag}"

tag_commit="$(git rev-parse "${tag}^{commit}")"
if [[ "${tag_commit}" != "${expected_commit}" ]]; then
    echo "checked-out commit differs from the signed tag" >&2
    exit 1
fi
if ! git merge-base --is-ancestor "${tag_commit}" "${main_ref}"; then
    echo "release tag commit is not on ${main_ref}" >&2
    exit 1
fi
