# ADR 0009: Windows/Linux platform scope

- Date: 2026-10-17
- Status: accepted user-directed scope amendment
- Reviewed base: `21239e7` on `main`
- Owners: 01 (CPU/build policy), 02 (remaining graphics qualification), 31/35 (client packaging/release), M01-M10 (Linux headless server and MMO gates)

## Decision

The user instructed: “disregard any mac work/requirements, from now on do not
worry about mac combatibility.” Shark therefore targets Windows and Ubuntu Linux
for clients and the editor, and Ubuntu Linux for its headless server. Mac support
on Apple Silicon and Intel is removed from current scope, including CPU CI,
graphics qualification, tooling, packaging, signing, driver translation and
ongoing compatibility maintenance. This supersedes the earlier temporary 02c
deferral and conflicting Mac obligations in the original roadmap and ADRs.

The seamless open-world MMORPG and equal human/terminal-agent authoring contracts
remain intact. [ADR 0003](0003-platforms.md) retains the exact Windows/Linux
OS/CPU/GPU baselines, Vulkan feature profile, readback tolerances and lifecycle
workload. [ADR 0007](0007-mmorpg-architecture.md) retains the Linux server boundary
and future authority, persistence, streaming and load requirements.

## Assignment and evidence consequences

Checkpoint **02c is retired**, not passed or deferred pending Mac access. Original
assignment IDs, the M01-M10 insertion order and the current assignment **02** remain
unchanged. The active graphics hardware matrix has six rows: W-NV, W-AMD,
W-INTEL, L-NV, L-AMD and L-INTEL. **All six remain NOT_RUN as complete mandatory
rows.** The supplemental Windows NVIDIA/Intel results do not qualify the named
baseline devices. Clean-target packaged execution and the remaining full hardware
matrix still block assignment 02 closure and dependent renderer work.

The former M-APPLE, M-INTEL-AMD and M-INTEL-IGPU rows no longer require evidence.
Their removal is a scope decision and makes no claim that the stack works on Mac.
Historical handoffs, receipts and recorded CPU red/green results retain their
original revisions, hashes, counts and platform statements; they describe the
scope at the time of execution. In particular, the
[02c deferral](../handoffs/02c-mac-deferred.md) and
[02d receipt](../coverage/evidence/02d-windows-local.json) are historical evidence,
not current Mac obligations. [REQ-02](../coverage/requirements.json) stays NOT_RUN.

## Implementation and maintenance

Remove active Mac build presets, CI jobs and required-check expectations. Keep
Windows/Linux CPU and sanitizer checks, contract/format checks and Linux headless
validation. Update planning documents, requirements and repository guidance so
future increments do not spend work qualifying or preserving Mac compatibility.
Mac-only dependency/tool candidates are not selected for shipping.

Generic portability branches, upstream source/license metadata and historical
artifacts may remain where their deletion would create unrelated churn. Their
presence does not promise a maintained build or runtime route on Mac. No new Mac
tools, compatibility patches or native runs are required by current assignments.
Any future platform addition requires its own explicit scope decision and native
qualification; it cannot inherit support from these retained records.

This amendment changes product scope and validation configuration. It does not
add engine features, qualify shipping platforms, weaken remaining GPU tests or
advance to assignment 03. The next work is the remaining Windows/Linux 02d
qualification in the [graphics checkpoint plan](../roadmap/graphics-plan.md).
