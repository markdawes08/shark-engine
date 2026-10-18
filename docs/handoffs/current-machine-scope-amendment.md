# Current-machine scope and roadmap reassessment

Date: 2026-10-17 local (inventory/evidence recorded 2026-10-18 UTC).
Base: `2ef281b` on `main`, initially clean.
Result: uncommitted planning/contract changes. The user owns all staging,
committing, pushing and release tagging.

## User direction and resulting plan

The user asked to limit the engine gameplan to what is currently available on
this machine. [ADR 0010](../architecture/0010-current-machine-scope.md) records
the accepted scope. Windows 11 build 26200, an i9-14900HX, approximately 32 GB RAM,
RTX 4070 Laptop (8,188 MiB reported VRAM), Intel UHD/Raptor Lake-S Mobile graphics
and existing WSL2 Ubuntu are the complete required development environment.

- Windows is the editor/client target; both actual GPUs retain correctness and
  lifecycle checks. RTX is the primary performance target, with a separately
  measured Intel fallback profile.
- WSL supplies Linux CPU/headless Server, persistence and isolated package tests.
  Native Linux graphical deployment and additional machines/GPUs are outside scope.
- Local multi-process and seeded network-fault fixtures retain authentication,
  server authority, transactional durability, seamless region transfer and recovery.
- LOCAL-MMO-1 replaces the former large-population gate: 16 active synthetic
  players, two region processes, 16 NPCs, a 16-player hotspot, 32 visible animated
  actors, a separate two-graphical-client fixture, 100 crossings and a two-hour
  active soak. All of these future behavior gates remain NOT_RUN.
- Package acceptance uses actual relocated Windows execution and separate SDK-free
  WSL userspaces. It does not claim an independent physical clean-machine test.
- Final release means a local development candidate. Cloud hosting, real WAN,
  independent-host failure, production scale/HA and public certificate-backed
  distribution are outside current acceptance. Existing hosted CI remains
  supplemental; its workflow and GitHub branch protection are unchanged.

No engine feature was removed merely to fit a workload. The seamless open-world
MMORPG direction, PBR/physics/audio/content goals and shared human/editor and
terminal-agent authoring contract remain. Future implementation must measure
resource use and establish bounded workloads before acceptance; the hardware
inventory itself is not capacity evidence.

## Existing-work assessment and assignment 02

The [scope receipt](../coverage/evidence/current-machine-scope.json) rechecks all
27 Windows and 14 Linux recorded source hashes against this checkout and pins the
unchanged [Windows](../coverage/evidence/02d-windows-refresh.json) and
[Linux](../coverage/evidence/02d-linux-local.json) receipts. Both local GPUs passed
100 resize/minimize/restore cycles, 20 device cycles and at least 600 presentation
seconds, with 14 readback checks, 11 artifacts each and zero validation warnings
or errors. Actual package relocation, missing/corrupt input rejection and watchdog
cleanup supplement the native reports. Linux build/CPU/SDK-free startup evidence
supports the retained userspace scope.

**REQ-02 is PASS under the explicitly amended local scope. Assignment 03 is next
and remains NOT_RUN/unimplemented.** The old six GPU rows remain historically
NOT_RUN; their requirements are retired, not passed. Native P02-07/P02-08 aggregate
fields are untouched; the new assessment uses their separate package/failure
fixtures. No new GPU run is claimed, and no full-production support claim follows.
The earlier resize timeout remains preserved and unexplained.

The target separation, existing Windows Vulkan/NVRHI probe and Linux headless
bootstrap fit the new scope and need no runtime rewrite. Keep Linux graphics
packaging as a useful CPU/dependency regression fixture; it no longer creates a
native Linux client support obligation. The build presets, dependency lock,
shaders, C++ implementation and hosted workflow are unchanged. No host packages,
drivers, WSL settings or remote repository policies were changed.

## Updated contracts

ADRs 0002/0003/0006/0007/0008, the architecture index, graphics/MMORPG/authoring
plans, README, AGENTS, build/probe/CI guides and relevant repository skills now
describe the local gates. ADR 0009 retains its historical text with a superseding
scope note. Earlier handoffs and raw evidence are unchanged.

The schema-3 requirement manifest adds `qualification_profile` and
`scope_document`, preserves all 46 assignment IDs/order and 50 requirements, and
updates affected acceptance criteria through 35. Component/authoring-command/Lua
arrays remain empty because those APIs do not exist. The contract validator binds
the local 02 PASS to the exact scope and unchanged evidence file hashes, rejects
missing/tampered receipts and retains future-assignment rejection. This validation
checks recorded contracts/evidence integrity; it does not execute a GPU or prove
future engine behavior.

The ignored local roadmap has the same scope and next assignment. `.gitignore`
already covers that roadmap, builds, caches and generated inventory; no additional
ignore rule is needed. New ADR/handoff/evidence and repository skills stay eligible
for version control.

## Verification

Inventory commands: `Get-CimInstance Win32_Processor`, `Win32_ComputerSystem`,
`Win32_OperatingSystem`, `Win32_VideoController`; `Get-Volume -DriveLetter C`;
`wsl --list --verbose`; and
`nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader`.
Only non-secret resource fields were retained. Available disk space is a snapshot,
not a new capacity requirement.

| Command from the repository root | Result |
| --- | --- |
| `.tools/venv/Scripts/python.exe tools/validate_contract.py --self-test` | PASS: 29 negative fixtures, local evidence pins, future-PASS rejection, 50 requirements/46 owners and document links |
| `.tools/venv/Scripts/python.exe tools/check_repository.py --self-test` | PASS: five skill sources, three malformed-metadata fixtures, source/ignore boundaries |
| `.tools/venv/Scripts/python.exe tools/check_ci_config.py` | PASS: retained nine-job workflow syntax/wiring; no hosted execution claimed |
| `.tools/venv/Scripts/python.exe out/current-machine-scope/record_scope.py` | PASS: unchanged receipt digests and all 41 recorded source-hash comparisons, both full local GPU workloads |
| `git diff --check` | PASS |

Generated command logs are in `out/current-machine-scope/`. The durable scope
receipt retains the non-secret inventory, original receipt references/digests,
source comparisons and exact local GPU/workload reassessment. An independent
review checked the active platform, capacity, packaging and progression wording.

Runtime builds, GPU soaks and hosted jobs are not rerun for this planning-only
change. Their immutable prior results remain tied to their tested inputs; current
source hashes were checked before reusing them. No new MMO, editor or release
behavior has been qualified.

## Next assignment

Proceed to **03: core services and application lifecycle** when requested. There
is no remaining requirement to obtain other hardware or a hosted run before
starting it. Implement the owned error/log/configuration/identity/path/lifecycle
contracts with Windows and WSL CPU tests and the existing Core/Server boundary.
This amendment does not itself start assignment 03.

Suggested commit: `docs(plan): scope Shark development to the current Windows PC`
