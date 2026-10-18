"""Validate Shark's requirement mapping and documentation, not engine behavior."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/coverage/requirements.json"
BASE_OWNERS = [f"{number:02d}" for number in range(36)]
MMO_OWNERS = [f"M{number:02d}" for number in range(1, 11)]
OWNERS = set(BASE_OWNERS + MMO_OWNERS)
# Insertions retain the original numeric order while closing MMO foundations
# before later layers depend on them. The manifest records the full sequence.
INSERT_AFTER = {
    "M01": "01", "02": "M01", "M02": "07", "08": "M02",
    "M03": "08", "09": "M03", "M04": "21", "M05": "M04",
    "M06": "M05", "22": "M06", "M07": "24", "25": "M07",
    "M08": "31", "32": "M08", "M09": "33", "34": "M09",
    "M10": "34", "35": "M10",
}
SURFACES = {"features", "components", "commands", "lua_api"}
DOCUMENTS = (
    "README.md",
    "AGENTS.md",
    "docs/building.md",
    "docs/ci.md",
    "docs/architecture/README.md",
    "docs/architecture/0001-targets-and-boundaries.md",
    "docs/architecture/0002-dependencies.md",
    "docs/architecture/0003-platforms.md",
    "docs/architecture/0004-formats-and-identity.md",
    "docs/architecture/0005-simulation-and-rendering.md",
    "docs/architecture/0006-v1-profile.md",
    "docs/architecture/0007-mmorpg-architecture.md",
    "docs/architecture/0008-human-agent-authoring.md",
    "docs/roadmap/mmorpg-plan.md",
    "docs/roadmap/human-agent-plan.md",
    "docs/roadmap/graphics-plan.md",
    "docs/graphics-probe.md",
    "docs/coverage/README.md",
    "docs/handoffs/00-architecture-contract.md",
    "docs/handoffs/mmorpg-planning-amendment.md",
    "docs/handoffs/M01-headless-server.md",
    "docs/handoffs/M01-ci-verification.md",
    "docs/handoffs/02a-graphics-setup.md",
    "docs/handoffs/02b-graphics-cases.md",
    "docs/handoffs/02c-mac-deferred.md",
    "docs/handoffs/02d-windows-validation.md",
    "docs/handoffs/human-agent-planning-amendment.md",
)


def nonempty_text(value: object) -> bool:
    return isinstance(value, str) and bool(value.strip())


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def validate_manifest(data: object) -> list[str]:
    errors = []
    if not isinstance(data, dict):
        return ["manifest must be an object"]
    if type(data.get("schema_version")) is not int or data["schema_version"] != 3:
        errors.append("schema_version must be integer 3")
    if data.get("product_profile") != "seamless-open-world-mmorpg":
        errors.append("product_profile must be seamless-open-world-mmorpg")
    if data.get("planning_document") != "docs/roadmap/mmorpg-plan.md":
        errors.append("planning_document must identify the versioned MMORPG plan")
    order = data.get("assignment_order")
    positions = {}
    if not isinstance(order, list) or not all(isinstance(value, str) for value in order):
        errors.append("assignment_order must be an array of assignment IDs")
    elif len(order) != len(OWNERS) or set(order) != OWNERS:
        errors.append("assignment_order must contain every assignment exactly once")
    else:
        positions = {owner: index for index, owner in enumerate(order)}
        if [owner for owner in order if owner in BASE_OWNERS] != BASE_OWNERS:
            errors.append("assignment_order must retain original 00-35 order")
        for owner, prerequisite in INSERT_AFTER.items():
            if positions[owner] <= positions[prerequisite]:
                errors.append(f"assignment_order: {owner} must follow {prerequisite}")
    assignment = data.get("current_assignment")
    if not isinstance(assignment, str) or assignment not in OWNERS:
        errors.append("current_assignment must be 00-35 or M01-M10")
        assignment = "00"
    if data.get("engine") != "Shark":
        errors.append("engine must be Shark")
    if data.get("roadmap") != "AI_Game_Engine_Agent_Roadmap.txt":
        errors.append("roadmap must identify the ignored local assignment document")
    requirements = data.get("requirements")
    if not isinstance(requirements, list) or not requirements:
        return errors + ["requirements must be a nonempty array"]

    seen_ids = set()
    seen_owners = set()
    for index, entry in enumerate(requirements):
        context = f"requirements[{index}]"
        if not isinstance(entry, dict):
            errors.append(f"{context} must be an object")
            continue
        identifier = entry.get("id")
        if not isinstance(identifier, str) or not re.fullmatch(r"REQ-[A-Z0-9-]+", identifier):
            errors.append(f"{context}: invalid requirement id")
        elif identifier in seen_ids:
            errors.append(f"{context}: duplicate requirement id {identifier}")
        else:
            seen_ids.add(identifier)
        owner = entry.get("owner")
        if not isinstance(owner, str) or owner not in OWNERS:
            errors.append(f"{context}: owner must be 00-35 or M01-M10")
        else:
            seen_owners.add(owner)
        for field in ("requirement", "planned_fixture"):
            if not nonempty_text(entry.get(field)):
                errors.append(f"{context}: {field} must be nonempty text")
        criteria = entry.get("acceptance")
        if not isinstance(criteria, list) or not criteria or not all(map(nonempty_text, criteria)):
            errors.append(f"{context}: acceptance must contain nonempty criteria")
        verification = entry.get("verification")
        if not isinstance(verification, dict):
            errors.append(f"{context}: verification must be an object")
            continue
        status = verification.get("status")
        if status not in ("PASS", "FAIL", "NOT_RUN"):
            errors.append(f"{context}: invalid verification status")
        if isinstance(owner, str) and owner in positions and positions[owner] > positions[assignment] and status != "NOT_RUN":
            errors.append(f"{context}: future assignment must remain NOT_RUN")
        if not nonempty_text(verification.get("reason")):
            errors.append(f"{context}: verification reason must be nonempty text")
    missing = sorted(OWNERS - seen_owners)
    if missing:
        errors.append("missing assignment owners: " + ", ".join(missing))

    surface = data.get("implemented_surface")
    if not isinstance(surface, dict) or set(surface) != SURFACES:
        errors.append("implemented_surface must contain features, components, commands and lua_api")
    else:
        public_ids = set()
        for category, entries in surface.items():
            if not isinstance(entries, list):
                errors.append(f"{category}: surface entries must be an array")
                continue
            for entry in entries:
                if not isinstance(entry, dict):
                    errors.append(f"{category}: surface entry must be an object")
                    continue
                identifier = entry.get("id")
                if not nonempty_text(identifier) or identifier in public_ids:
                    errors.append(f"{category}: surface entry needs a unique id")
                else:
                    public_ids.add(identifier)
                if not nonempty_text(entry.get("name")):
                    errors.append(f"{category}: surface entry needs a name")
                references = entry.get("requirement_ids")
                if not isinstance(references, list) or not references or any(
                    not isinstance(ref, str) or ref not in seen_ids for ref in references
                ):
                    errors.append(f"{category}: surface entry needs existing requirement_ids")
                for field in ("test_ids", "test_sources", "fixtures"):
                    values = entry.get(field)
                    if not isinstance(values, list) or not values or not all(map(nonempty_text, values)):
                        errors.append(f"{category}: surface entry needs nonempty {field}")
                if not nonempty_text(entry.get("fixture_reason")):
                    errors.append(f"{category}: surface entry needs a fixture_reason")
    return errors


def validate_documents(root: Path) -> list[str]:
    errors = []
    # The user keeps the original roadmap local and ignored. CI uses the versioned
    # requirements/ADRs; absence of that private input cannot break a clean clone.
    for relative in DOCUMENTS:
        if not (root / relative).is_file():
            errors.append(f"missing document: {relative}")
    files = [root / "README.md", root / "AGENTS.md", *sorted((root / "docs").rglob("*.md")), *sorted((root / ".agents/skills").rglob("*.md"))]
    for path in files:
        if not path.is_file():
            continue
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            errors.append(f"cannot read {path.relative_to(root)}: {error}")
            continue
        for match in re.finditer(r"\[[^\]]*\]\(([^)]+)\)", source):
            target = match.group(1).strip().strip("<>")
            if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target) or target.startswith("#"):
                continue
            destination = (path.parent / target.split("#", 1)[0]).resolve()
            if not destination.is_relative_to(root) or not destination.exists():
                errors.append(f"broken local link in {path.relative_to(root)}: {target}")
    return errors


def validate_surface_files(data: dict, root: Path) -> list[str]:
    errors = []
    for category, entries in data["implemented_surface"].items():
        for entry in entries:
            for relative in entry["test_sources"] + entry["fixtures"]:
                path = (root / relative).resolve()
                if not path.is_relative_to(root) or not path.is_file():
                    errors.append(f"{category}/{entry['id']}: missing test or fixture: {relative}")
    return errors


def self_test(data: dict) -> list[str]:
    """In-memory mutations prove omissions/false claims do not pass the gate."""
    # Keep the fixtures stable as real assignments advance and gain evidence.
    # Production validation above still checks the caller's actual state first.
    data = copy.deepcopy(data)
    data["current_assignment"] = "01"
    for requirement in data["requirements"]:
        requirement["verification"]["status"] = "NOT_RUN"
    cases = []

    def changed(label, change, expected):
        candidate = copy.deepcopy(data)
        change(candidate)
        cases.append((label, validate_manifest(candidate), expected))

    changed("missing assignment", lambda d: d.update(requirements=[r for r in d["requirements"] if r["owner"] != "35"]), "missing assignment owners: 35")
    changed("duplicate id", lambda d: d["requirements"].append(copy.deepcopy(d["requirements"][0])), "duplicate requirement id")
    changed("missing criterion", lambda d: d["requirements"][0].update(acceptance=[]), "acceptance must contain")
    changed("blank criterion", lambda d: d["requirements"][0].update(acceptance=[" "]), "acceptance must contain")
    changed("invalid owner", lambda d: d["requirements"][0].update(owner="36"), "owner must be")
    changed("missing MMO owner", lambda d: d.update(requirements=[r for r in d["requirements"] if r["owner"] != "M05"]), "missing assignment owners: M05")
    changed("missing ordered assignment", lambda d: d["assignment_order"].remove("M05"), "assignment_order must contain")
    changed("duplicate ordered assignment", lambda d: d["assignment_order"].append("01"), "assignment_order must contain")
    changed("misordered MMO dependency", lambda d: d["assignment_order"].reverse(), "must follow")
    changed("false future MMO pass", lambda d: next(r for r in d["requirements"] if r["owner"] == "M01")["verification"].update(status="PASS"), "future assignment must remain NOT_RUN")
    changed("wrong product profile", lambda d: d.update(product_profile="offline-only"), "product_profile must be")
    changed("missing fixture", lambda d: d["requirements"][0].pop("planned_fixture"), "planned_fixture must be")
    changed("false future pass", lambda d: next(r for r in d["requirements"] if r["owner"] == "02")["verification"].update(status="PASS"), "future assignment must remain NOT_RUN")
    changed("missing evidence reason", lambda d: d["requirements"][0]["verification"].update(reason=""), "verification reason must be")
    changed("fake implemented API", lambda d: d["implemented_surface"]["lua_api"].append("fake_api"), "surface entry must be an object")
    changed("missing surface category", lambda d: d["implemented_surface"].pop("commands"), "implemented_surface must contain")
    changed("invalid schema", lambda d: d.update(schema_version=True), "schema_version must be integer")
    changed("malformed requirement", lambda d: d["requirements"].append(None), "must be an object")
    cases.append(("non-object manifest", validate_manifest([]), "manifest must be an object"))
    try:
        json.loads('{"id": 1, "id": 2}', object_pairs_hook=unique_object)
        duplicate_errors = []
    except ValueError as error:
        duplicate_errors = [str(error)]
    cases.append(("duplicate JSON key", duplicate_errors, "duplicate JSON key"))

    failures = [label for label, errors, expected in cases if not any(expected in error for error in errors)]
    # M01 precedes 02 even though lexical string order would place it later.
    reordered = copy.deepcopy(data)
    reordered["current_assignment"] = "02"
    next(r for r in reordered["requirements"] if r["owner"] == "M01")["verification"]["status"] = "PASS"
    if validate_manifest(reordered):
        failures.append("completed earlier MMO assignment rejected by lexical ordering")
    if not failures:
        print(f"PASS: {len(cases)} negative checks reject omissions, malformed data and false claims")
        print("PASS: completed MMO insertion follows declared order rather than lexical ID order")
    return [f"negative check did not fail as expected: {label}" for label in failures]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="also verify rejection of invalid contract data")
    arguments = parser.parse_args()
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"), object_pairs_hook=unique_object)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"FAIL: cannot load requirements manifest: {error}", file=sys.stderr)
        return 1
    errors = validate_manifest(data) + validate_documents(ROOT)
    if not errors:
        errors.extend(validate_surface_files(data, ROOT))
    if not errors and arguments.self_test:
        errors.extend(self_test(data))
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {len(data['requirements'])} requirements cover 46 ordered assignments (00-35 and M01-M10); document links resolve")
    print("Scope: requirement/documentation checks only; CTest and hardware qualification are separate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
