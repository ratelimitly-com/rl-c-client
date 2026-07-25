#!/usr/bin/env python3
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys


SEMVER = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Write the deterministic rl-c-client dependency SPDX SBOM"
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--project-version", required=True)
    parser.add_argument("--openssl-version", required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    if not SEMVER.fullmatch(args.project_version):
        raise ValueError("--project-version must be numeric MAJOR.MINOR.PATCH")
    if not SEMVER.fullmatch(args.openssl_version):
        raise ValueError("--openssl-version must be numeric MAJOR.MINOR.PATCH")

    epoch_value = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch_value is None or not epoch_value.isdigit():
        raise ValueError("SOURCE_DATE_EPOCH must be a non-negative integer")
    epoch = int(epoch_value)
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )

    document = {
        "SPDXID": "SPDXRef-DOCUMENT",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: rl-c-client/write_dependency_sbom.py"],
        },
        "dataLicense": "CC0-1.0",
        "documentNamespace": (
            "https://github.com/ratelimitly-com/rl-c-client/"
            f"spdx/dependencies/{args.project_version}/openssl-"
            f"{args.openssl_version}"
        ),
        "name": f"rl-c-client-{args.project_version}-dependencies",
        "packages": [
            {
                "SPDXID": "SPDXRef-Package-OpenSSL",
                "copyrightText": "Copyright OpenSSL contributors",
                "downloadLocation": (
                    f"https://www.openssl.org/source/openssl-"
                    f"{args.openssl_version}.tar.gz"
                ),
                "externalRefs": [
                    {
                        "referenceCategory": "PACKAGE-MANAGER",
                        "referenceLocator": (
                            f"pkg:generic/openssl@{args.openssl_version}"
                        ),
                        "referenceType": "purl",
                    }
                ],
                "filesAnalyzed": False,
                "licenseConcluded": "Apache-2.0",
                "licenseDeclared": "Apache-2.0",
                "name": "OpenSSL",
                "versionInfo": args.openssl_version,
            },
            {
                "SPDXID": "SPDXRef-Package-rl-c-client",
                "copyrightText": "NOASSERTION",
                "downloadLocation": (
                    "https://github.com/ratelimitly-com/rl-c-client"
                ),
                "filesAnalyzed": False,
                "licenseConcluded": "MIT",
                "licenseDeclared": "MIT",
                "name": "rl-c-client",
                "versionInfo": args.project_version,
            },
        ],
        "relationships": [
            {
                "relatedSpdxElement": "SPDXRef-Package-rl-c-client",
                "relationshipType": "DESCRIBES",
                "spdxElementId": "SPDXRef-DOCUMENT",
            },
            {
                "relatedSpdxElement": "SPDXRef-Package-OpenSSL",
                "relationshipType": "STATIC_LINK",
                "spdxElementId": "SPDXRef-Package-rl-c-client",
            },
        ],
        "spdxVersion": "SPDX-2.2",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, OverflowError, ValueError) as error:
        print(f"write_dependency_sbom: {error}", file=sys.stderr)
        sys.exit(1)
