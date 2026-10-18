"""Compatibility entry point for preparing the locked EnTT archive from 07a.

New Core builds require both EnTT and GLM; use core_dependencies.py to prepare both.
"""

import argparse
import hashlib
import json
from pathlib import Path
import tarfile

from core_dependencies import archive_files, entry, prepare, verify_sources


def verify_build(source, build, archive):
    return verify_sources(source, build, archive, "EnTT")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        result = prepare(Path(__file__).resolve().parents[1], options.output.absolute())
        print(json.dumps({"status": "PASS", "archive": str(result),
                          "sha256": hashlib.sha256(result.read_bytes()).hexdigest()}))
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        raise SystemExit(1)
