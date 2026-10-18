"""Check repository skill discovery inputs and intentional Git ignore boundaries."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys

import yaml


ROOT = Path(__file__).resolve().parents[1]
SKILLS = (
    "shark-build-test",
    "shark-renderer-validation",
    "shark-component-api",
    "shark-asset-import",
    "shark-release-packaging",
)
IGNORED = (
    "AI_Game_Engine_Agent_Roadmap.txt",
    "build/example/Runtime.exe",
    "out/ci/windows-debug/junit.xml",
    ".tools/venv/pyvenv.cfg",
    ".tools/headless/base.tar.gz",
    "out/ci/headless/closure.json",
    "out/server-package/bin/Server",
    "CMakeUserPresets.json",
    ".env",
)
VISIBLE = (
    ".gitignore", "AGENTS.md", "CMakeLists.txt", "CMakePresets.json",
    "requirements-dev.txt", "dependencies.lock.json", "docs/coverage/requirements.json",
    ".github/workflows/cpu.yml", "assets/FeatureWorld/mesh.bin",
    "engine/core/src/server_config.cpp", "engine/client/src/bootstrap.cpp",
    "server/main.cpp", "tools/check_server_closure.py",
    "assets/FeatureWorld/reference.png", "assets/FeatureWorld/mesh.obj",
    *(f".agents/skills/{name}/SKILL.md" for name in SKILLS),
)


def validate_skill(source: str, folder_name: str) -> list[str]:
    parts = source.split("---", 2)
    if len(parts) != 3 or parts[0].strip():
        return ["missing YAML frontmatter"]
    try:
        metadata = yaml.safe_load(parts[1])
    except yaml.YAMLError as error:
        return [f"invalid skill YAML: {error}"]
    if not isinstance(metadata, dict):
        return ["skill metadata must be an object"]
    errors = []
    name = metadata.get("name")
    if name != folder_name or not re.fullmatch(r"[a-z0-9-]{1,63}", folder_name):
        errors.append("name must match the discovery folder")
    description = metadata.get("description")
    if not isinstance(description, str) or not description.strip():
        errors.append("missing discovery description")
    if not parts[2].strip():
        errors.append("missing skill instructions")
    return errors


def check_ignored(path: str) -> bool:
    result = subprocess.run(
        ["git", "check-ignore", "--no-index", "--quiet", "--", path],
        cwd=ROOT, capture_output=True, text=True, timeout=15,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"git check-ignore failed for {path}: {result.stderr.strip()}")
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    errors = []
    try:
        for required in ("CMakeLists.txt", "CMakePresets.json", "dependencies.lock.json", "requirements-dev.txt", ".github/workflows/cpu.yml"):
            if not (ROOT / required).is_file():
                errors.append(f"missing foundation file: {required}")
        guidance = (ROOT / "AGENTS.md").read_text(encoding="utf-8")
        for name in SKILLS:
            relative = f".agents/skills/{name}/SKILL.md"
            source = (ROOT / relative).read_text(encoding="utf-8")
            errors.extend(f"{relative}: {error}" for error in validate_skill(source, name))
            if relative not in guidance:
                errors.append(f"AGENTS.md does not link discovery entry {relative}")
        for path in IGNORED:
            if not check_ignored(path):
                errors.append(f"generated/local file is not ignored: {path}")
        for path in VISIBLE:
            if check_ignored(path):
                errors.append(f"source, guidance or asset path is incorrectly ignored: {path}")
    except (OSError, UnicodeError, RuntimeError, subprocess.TimeoutExpired) as error:
        errors.append(str(error))
    if arguments.self_test:
        valid = "---\nname: shark-build-test\ndescription: Build Shark.\n---\nRun the native build.\n"
        if validate_skill(valid, "shark-build-test"):
            errors.append("self-test rejected valid discovery metadata")
        for invalid in ("No frontmatter", valid.replace("name: shark-build-test", "name: wrong-name"), valid.replace("description: Build Shark.", "description: ''")):
            if not validate_skill(invalid, "shark-build-test"):
                errors.append("self-test accepted invalid discovery metadata")
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS: five discoverable skill sources, AGENTS links and Git ignore boundaries")
    print("Source files are eligible for version control; staging and commits belong to the user")
    if arguments.self_test:
        print("PASS: valid metadata accepted; three malformed skill cases rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
