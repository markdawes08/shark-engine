"""Prepare and verify the fixed reviewed Core archives; no package resolution."""

from __future__ import annotations

import hashlib
import argparse
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tarfile
import tempfile
import urllib.request


MAX_ARCHIVE = 8 * 1024 * 1024
MAX_CONTENT = 32 * 1024 * 1024
# WinNT.h IsReparseTagNameSurrogate: junctions and other redirected names.
WINDOWS_NAME_SURROGATE = 0x20000000
MBEDTLS_LICENSE_FILES = (
    "LICENSE", "framework/LICENSE", "tf-psa-crypto/LICENSE", "tf-psa-crypto/framework/LICENSE",
    "tf-psa-crypto/drivers/everest/library/Hacl_Curve25519.c",
    "tf-psa-crypto/drivers/everest/library/legacy/Hacl_Curve25519.c",
    "tf-psa-crypto/drivers/everest/include/tf-psa-crypto/private/everest/kremlib.h",
    "tf-psa-crypto/drivers/pqcp/mldsa-native/LICENSE",
)
DEPENDENCIES = {
    "EnTT": {"prefix": "entt", "license": "LICENSE", "header": "src/entt/entity/registry.hpp",
             "cache": "SHARK_EnTT_ARCHIVE"},
    "GLM": {"prefix": "glm", "license": "copying.txt", "header": "glm/glm.hpp",
            "cache": "SHARK_GLM_ARCHIVE_INPUT"},
    "MbedTLS": {"prefix": "mbedtls", "license": "LICENSE", "header": "include/mbedtls/ssl.h",
                "cache": "SHARK_MbedTLS_ARCHIVE_INPUT"},
    "Jolt": {"prefix": "jolt", "license": "LICENSE", "header": "Jolt/Jolt.h",
             "cache": "SHARK_JOLT_ARCHIVE_INPUT"},
}


def specification(name: str) -> dict:
    if name not in DEPENDENCIES:
        raise ValueError("Unreviewed Core dependency: " + name)
    return DEPENDENCIES[name]


def entry(source: Path, name: str = "EnTT") -> dict:
    if name == "Jolt":
        from physics_dependency import entry as physics_entry
        return physics_entry(source)
    spec = specification(name)
    result = json.loads((source / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"][name]
    licenses = list(MBEDTLS_LICENSE_FILES) if name == "MbedTLS" else [spec["license"]]
    license_name = "Apache-2.0" if name == "MbedTLS" else "MIT"
    if (result.get("license") != license_name or result.get("license_files") != licenses
            or result.get("transitive_dependencies") != [] or result.get("patches") != []):
        raise ValueError(name + " dependency metadata requires a new integration review")
    return result


def archive_files(archive: Path, locked: dict, name: str = "EnTT") -> dict[str, bytes]:
    if name == "Jolt":
        from physics_dependency import archive_files as physics_archive_files
        return physics_archive_files(archive, locked)
    spec = specification(name)
    if (not archive.is_file() or archive.is_symlink() or archive.stat().st_size > MAX_ARCHIVE):
        raise ValueError("Missing, linked or oversized " + name + " archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError(name + " archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    prefix = locked["archive_prefix"] if name == "MbedTLS" else spec["prefix"] + "-" + locked["commit"]
    links = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as stream:
        for member in stream:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != prefix):
                raise ValueError("Unsafe " + name + " archive member")
            if member.issym() and name == "MbedTLS":
                # The exact release has 147 links solely in disabled PQC test/example
                # trees. Omit them portably; no build input follows or materializes
                # a link, including on Windows without symlink privileges.
                relative = path.relative_to(prefix).as_posix()
                if (not relative.startswith("tf-psa-crypto/drivers/pqcp/mldsa-native/")
                        or not any(part in path.parts for part in ("test", "examples"))):
                    raise ValueError("Unreviewed MbedTLS source symlink")
                links += 1
                continue
            if not (member.isfile() or member.isdir()):
                raise ValueError("Unsafe " + name + " archive member")
            if member.isdir():
                continue
            relative = path.relative_to(prefix).as_posix()
            total += member.size
            content_limit = 64 * 1024 * 1024 if name == "MbedTLS" else MAX_CONTENT
            if relative in files or len(files) >= 4096 or total > content_limit:
                raise ValueError("Duplicate or oversized " + name + " archive contents")
            files[relative] = stream.extractfile(member).read()
    if spec["license"] not in files or spec["header"] not in files:
        raise ValueError("Incomplete " + name + " source archive")
    if name == "MbedTLS" and (links != 147 or any(item not in files for item in MBEDTLS_LICENSE_FILES)):
        raise ValueError("MbedTLS release inventory requires a new integration review")
    return files


def prepare(source: Path, destination: Path, name: str = "EnTT") -> Path:
    """Explicit preparation before a disconnected configure, outside its graph."""
    if name == "Jolt":
        from physics_dependency import prepare as prepare_physics
        return prepare_physics(source, destination)
    locked = entry(source, name)
    destination.mkdir(parents=True, exist_ok=True)
    filename = "mbedtls-" + locked["version"] + ".tar.bz2" if name == "MbedTLS" else locked["commit"] + ".tar.gz"
    archive = destination / filename
    if archive.exists():
        archive_files(archive, locked, name)
        return archive
    with urllib.request.urlopen(locked["url"], timeout=60) as response:
        data = response.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE or hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Downloaded " + name + " archive differs from the lock or exceeds its limit")
    # Exclusive creation preserves any concurrently provided cache file.
    with archive.open("xb") as stream:
        stream.write(data)
    archive_files(archive, locked, name)
    return archive


def verify_sources(source: Path, build: Path, archive: Path, name: str = "EnTT") -> dict:
    """Compare the complete extracted source inventory with its verified archive."""
    if name == "Jolt":
        from physics_dependency import verify_sources as verify_physics_sources
        return verify_physics_sources(source, build, archive)
    locked = entry(source, name)
    files = archive_files(archive, locked, name)
    return _verify_source_files(build, archive, name, locked, files)


def _verify_source_files(build: Path, archive: Path, name: str,
                         locked: dict, files: dict[str, bytes]) -> dict:
    """Check every populated byte against this invocation's verified archive."""
    spec = specification(name)
    populated = build / "_deps" / (spec["prefix"] + "-src")
    try:
        root_metadata = populated.lstat()
    except FileNotFoundError:
        raise ValueError("Missing or linked " + name + " source directory") from None
    if (not stat.S_ISDIR(root_metadata.st_mode)
            or getattr(root_metadata, "st_reparse_tag", 0) & WINDOWS_NAME_SURROGATE):
        raise ValueError("Missing or linked " + name + " source directory")
    actual = {}
    for path in populated.rglob("*"):
        metadata = path.lstat()
        mode = metadata.st_mode
        if (stat.S_ISLNK(mode) or getattr(metadata, "st_reparse_tag", 0) & WINDOWS_NAME_SURROGATE
                or not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))):
            raise ValueError("Linked or nonregular " + name + " source input")
        if stat.S_ISREG(mode):
            relative = path.relative_to(populated).as_posix()
            if (relative not in files or metadata.st_size != len(files[relative])
                    or path.read_bytes() != files[relative]):
                raise ValueError("Populated " + name + " source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated " + name + " source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"],
            "archive": str(archive), "archive_sha256": locked["sha256"],
            "source_files": actual, "license_file": spec["license"],
            "license_sha256": hashlib.sha256(files[spec["license"]]).hexdigest(),
            "license_files": {item: hashlib.sha256(files[item]).hexdigest() for item in locked["license_files"]},
            "omitted_unbuilt_symlinks": 147 if name == "MbedTLS" else 0}


def populate_network(source: Path, build: Path, archive: Path) -> dict:
    """Materialize only the reviewed release's regular files; never repair drift."""
    name = "MbedTLS"
    locked = entry(source, name)
    files = archive_files(archive, locked, name)
    dependencies = build / "_deps"
    if dependencies.is_symlink():
        raise ValueError("Linked Core dependency directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    populated = dependencies / "mbedtls-src"
    if populated.is_symlink():
        raise ValueError("Linked MbedTLS source directory")
    if not populated.exists():
        with tempfile.TemporaryDirectory(prefix=".mbedtls-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            directories = {parent for relative in files for parent in Path(relative).parents
                           if parent != Path(".")}
            for directory in sorted(directories, key=lambda path: (len(path.parts), path.as_posix())):
                (candidate / directory).mkdir()
            for relative, data in files.items():
                path = candidate / relative
                path.write_bytes(data)
            os.replace(candidate, populated)
    return _verify_source_files(build, archive, name, locked, files)


def verify_build(source: Path, build: Path, archives: dict[str, Path]) -> dict:
    """The headless graph allows exactly the reviewed Core dependencies."""
    if set(archives) != set(DEPENDENCIES):
        raise ValueError("Missing locked Core archive evidence")
    dependencies = build / "_deps"
    expected = {spec["prefix"] + suffix for spec in DEPENDENCIES.values()
                for suffix in ("-src", "-build", "-subbuild")}
    expected.remove("mbedtls-subbuild")
    expected.remove("jolt-subbuild")
    if (not dependencies.is_dir() or dependencies.is_symlink()
            or {item.name for item in dependencies.iterdir()} != expected
            or any(item.is_symlink() or not item.is_dir() for item in dependencies.iterdir())):
        raise ValueError("Server-only dependency tree must contain exactly the locked Core populations")
    return {name: verify_sources(source, build, archive, name) for name, archive in archives.items()}


def main(names=tuple(DEPENDENCIES)) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--dependency", choices=tuple(DEPENDENCIES), action="append")
    parser.add_argument("--populate-network", type=Path, metavar="BUILD_DIR")
    parser.add_argument("--archive", type=Path)
    options = parser.parse_args()
    try:
        source = Path(__file__).resolve().parents[1]
        if options.populate_network:
            if not options.archive:
                parser.error("--populate-network requires --archive")
            record = populate_network(source, options.populate_network.absolute(), options.archive.absolute())
            print(json.dumps({"status": "PASS", "dependency": "MbedTLS", "archive_sha256": record["archive_sha256"],
                              "regular_files": len(record["source_files"]), "omitted_unbuilt_symlinks": 147}))
            return 0
        if not options.output:
            parser.error("--output is required for preparation")
        names = options.dependency or names
        archives = {name: prepare(source, options.output.absolute(), name) for name in names}
        print(json.dumps({"status": "PASS", "archives": {
            name: {"archive": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for name, path in archives.items()}}))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
