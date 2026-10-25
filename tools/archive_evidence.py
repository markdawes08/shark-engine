"""Create and verify portable, hash-manifested evidence archives; never delete inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import zipfile
import zlib


MANIFEST = "__shark_evidence_manifest__.json"
CHUNK = 1024 * 1024


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def relative_name(value):
    if not isinstance(value, str) or not value or any(c in value for c in "\\:\x00"):
        raise ValueError("Expected a portable relative file path")
    path = PurePosixPath(value)
    if path.is_absolute() or any(p in ("", ".", "..") for p in value.split("/")):
        raise ValueError(f"Unsafe relative path: {value}")
    return value


def reject_links(path):
    for candidate in (path, *path.parents):
        info = candidate.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError(f"Linked/reparse path is not supported: {candidate}")


def digest_stream(stream, destination=None):
    digest = hashlib.sha256()
    size = 0
    while block := stream.read(CHUNK):
        digest.update(block)
        size += len(block)
        if destination is not None:
            destination.write(block)
    return {"bytes": size, "sha256": digest.hexdigest()}


def verify_archive(archive):
    archive = Path(archive)
    with zipfile.ZipFile(archive) as bundle:
        names = [item.filename for item in bundle.infolist()]
        if len(names) != len(set(names)) or names.count(MANIFEST) != 1:
            raise ValueError("Archive has duplicate members or no unique manifest")
        for item in bundle.infolist():
            relative_name(item.filename)
            mode = item.external_attr >> 16
            if item.is_dir() or stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                raise ValueError("Archive members must be regular files")
        manifest = json.loads(bundle.read(MANIFEST), object_pairs_hook=unique_object)
        if not isinstance(manifest, dict) or set(manifest) != {"version", "files"} or type(manifest["version"]) is not int or manifest["version"] != 1:
            raise ValueError("Unsupported evidence manifest")
        files = manifest["files"]
        if not isinstance(files, dict) or not files or set(names) != {*files, MANIFEST}:
            raise ValueError("Manifest does not exactly match archive contents")
        for name, expected in files.items():
            relative_name(name)
            if name == MANIFEST or not isinstance(expected, dict) or set(expected) != {"bytes", "sha256"}:
                raise ValueError("Invalid file record")
            if type(expected["bytes"]) is not int or expected["bytes"] < 0 or not isinstance(expected["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", expected["sha256"]):
                raise ValueError("Invalid file digest/size")
            if bundle.getinfo(name).file_size != expected["bytes"]:
                raise ValueError(f"File size mismatch: {name}")
            with bundle.open(name) as stream:
                if digest_stream(stream) != expected:
                    raise ValueError(f"File hash mismatch: {name}")
    with archive.open("rb") as stream:
        archive_record = digest_stream(stream)
    return {"status": "PASS", "files": len(files),
            "source_bytes": sum(row["bytes"] for row in files.values()),
            "archive": str(archive), "archive_bytes": archive_record["bytes"],
            "archive_sha256": archive_record["sha256"]}


def create_archive(root, selection, output):
    root = Path(os.path.abspath(root))
    reject_links(root)
    if not root.is_dir() or not isinstance(selection, list) or not selection:
        raise ValueError("Expected an existing root and nonempty JSON file list")
    names = [relative_name(name) for name in selection]
    if len(names) != len(set(names)) or MANIFEST in names:
        raise ValueError("Selection has duplicate paths or the reserved manifest name")
    output = Path(os.path.abspath(output))
    reject_links(output.parent)
    if output.exists() or output.is_symlink():
        raise ValueError("Output already exists")
    paths = []
    for name in sorted(names):
        path = root.joinpath(*PurePosixPath(name).parts)
        reject_links(path)
        if not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"Not a regular file below root: {name}")
        if path == output:
            raise ValueError("Output cannot be an input")
        paths.append((name, path))
    created = False
    try:
        with output.open("xb") as destination:
            created = True
            with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6,
                                 allowZip64=True) as bundle:
                files = {}
                for name, path in paths:
                    reject_links(path)
                    with path.open("rb") as source:
                        before = os.fstat(source.fileno())
                        with bundle.open(name, "w", force_zip64=True) as target:
                            files[name] = digest_stream(source, target)
                        after = os.fstat(source.fileno())
                    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns) or files[name]["bytes"] != after.st_size:
                        raise ValueError(f"Input changed during archiving: {name}")
                bundle.writestr(MANIFEST, json.dumps({"version": 1, "files": files}, indent=2) + "\n")
            destination.flush()
            os.fsync(destination.fileno())
        return verify_archive(output)
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument("--root", type=Path, required=True)
    create.add_argument("--selection", type=Path, required=True, help="JSON list of exact root-relative file paths")
    create.add_argument("--output", type=Path, required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--archive", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        if arguments.command == "create":
            selection = json.loads(arguments.selection.read_text(encoding="utf-8"))
            result = create_archive(arguments.root, selection, arguments.output)
        else:
            result = verify_archive(arguments.archive)
        print(json.dumps(result))
        return 0
    except (OSError, ValueError, RuntimeError, EOFError, zipfile.BadZipFile, zlib.error) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
