#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="rl-dependency-sbom-") as tmp:
        output = Path(tmp) / "dependencies.spdx.json"
        env = os.environ.copy()
        env["SOURCE_DATE_EPOCH"] = "1700000000"
        subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "write_dependency_sbom.py"),
                "--output",
                str(output),
                "--project-version",
                "1.2.3",
                "--openssl-version",
                "3.6.3",
            ],
            check=True,
            env=env,
        )
        document = json.loads(output.read_text(encoding="utf-8"))
        assert document["spdxVersion"] == "SPDX-2.2"
        assert document["creationInfo"]["created"] == "2023-11-14T22:13:20Z"
        packages = {package["SPDXID"]: package for package in document["packages"]}
        assert packages["SPDXRef-Package-rl-c-client"]["versionInfo"] == "1.2.3"
        openssl = packages["SPDXRef-Package-OpenSSL"]
        assert openssl["versionInfo"] == "3.6.3"
        assert openssl["licenseConcluded"] == "Apache-2.0"
        assert openssl["licenseDeclared"] == "Apache-2.0"
        assert any(
            relationship["spdxElementId"]
            == "SPDXRef-Package-rl-c-client"
            and relationship["relationshipType"] == "STATIC_LINK"
            and relationship["relatedSpdxElement"]
            == "SPDXRef-Package-OpenSSL"
            for relationship in document["relationships"]
        )

    macos = (ROOT / "packaging" / "macos" / "build-sdk.sh").read_text(
        encoding="utf-8"
    )
    windows = (ROOT / "packaging" / "windows" / "build-sdk.ps1").read_text(
        encoding="utf-8"
    )
    for script in (macos, windows):
        assert "OpenSSL-LICENSE.txt" in script
        assert "write_dependency_sbom.py" in script
        assert "dependencies.spdx.json" in script

    print("test_dependency_sbom: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
