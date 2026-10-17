# Seamless MMORPG planning amendment handoff

Date: 2026-10-17. Scope: user-requested planning update and review of work through
assignment 01. This is not implementation of assignment 02 or an MMO subsystem.
The user chose **seamless open world** after requesting MMORPG specialization.

## Revision and ownership

Started from clean `main` at `3fecfc3` (`build: establish Shark build and test
foundation`), tracking `origin/main`. The user has committed/pushed since the
previous handoff. No new commit was created; resulting changes remain in the
working tree. Staging, commits and pushes remain exclusively user-owned.
Historical 00/01 handoffs retain their original evidence and starting state;
their old next-assignment statements are superseded by this plan.

## Decisions and existing-work assessment

The [versioned plan](../roadmap/mmorpg-plan.md) contains the full assessment,
execution order, ten inserted MMO assignments and amended original-owner gates.
[ADR 0007](../architecture/0007-mmorpg-architecture.md) defines server authority,
continuous-world ownership/streaming, persistence, trust boundaries and provisional
load targets. ADRs 0001-0006, README, AGENTS, build/CI guidance, coverage and all five
repository skills are aligned. The ignored local roadmap is amended, so its
old networking/animation/terrain exclusions cannot redirect a later agent.

The C++ bootstrap, dependency lock, CMake targets, tests and CPU CI remain useful
and require no rewrite now. Existing code has no simulation/network/persistence/
renderer implementation to migrate. Concrete M01 work is the Core/Client/Server
split plus headless dependency/CI checks; current `runtime-only` cannot prove it.
Database, transport, navigation and deployment integrations remain unselected.

The new release scope requires authoritative headless simulation, authenticated
sessions, interest-managed replication, durable/idempotent player state, seamless
cross-region transfer, streaming collision/navigation, animated crowds, a small
RPG reference module, operational recovery and active-load qualification. Original
client graphics, Mac feasibility, editor and public-tooling requirements remain.
Provisional 20 Hz/256-player and other budgets are engineering starting targets,
not user-approved final combat/capacity choices or measured performance.

## Changed contracts and coverage

Requirements schema 2 becomes 3: explicit product profile, versioned plan path,
46-entry assignment order and ten new owners/requirements (49 total). Existing
requirements acquire MMO constraints; future-pass checks use declared order.
The validator rejects missing/reordered owners and preserves honest future status.
All new gates are NOT_RUN. The two real foundation feature mappings are unchanged;
no server, gameplay API or test world is registered as implemented. No game asset,
wire message or database format has been created or migrated.

`.gitignore` was reviewed and remains sufficient for this change. The roadmap stays
ignored; plans/skills/fixtures stay eligible for version control. Generated local
editing/test artifacts remain under ignored `out/` and `.tools/`. Future agents
must add generated paths when actual subsystems introduce them.

## Verification

On the existing Windows11 x64 environment with Python3.13.5 and the pinned local
tools, these checks passed:

| Command/check | Result |
| --- | --- |
| `python tools/validate_contract.py --self-test` | PASS: 49 requirements, 46 ordered owners, document links, 20 negative cases and a positive non-lexical-order case |
| `python tools/check_repository.py --self-test` | PASS: five repository skills, guidance/ignore rules and three malformed-metadata cases |
| `powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --output-on-failure` | PASS: all 10 existing tests |
| Same wrapper with `ctest --preset windows-debug -L contract --output-on-failure` after the final validator fix | PASS: both contract/repository tests |
| Skill-creator `quick_validate.py` on each of the five edited skills | PASS; component skill rechecked after the final thread-rule correction |
| In-memory validator/self-test progress cases at 01, M01, 02, M05, M07 and 35 | PASS: self-tests remain valid as real assignments advance |
| `git diff --check`, `git diff --cached --exit-code`, `git check-ignore -v AI_Game_Engine_Agent_Roadmap.txt` | PASS: no whitespace errors or staged changes; local roadmap still ignored |

Two independent agents reviewed the foundation and amended contracts. Review
corrections cover per-region thread ownership, progress-independent validator
fixtures, M05 development payloads versus final cooking, later party/trade
integration, scheduler ownership and a consistent fenced transfer cut with
post-cut input/completion reconciliation. Existing original assignment acceptance
criteria remain, with added MMORPG constraints.

MMO behavior, load, hardware and hosted CI are NOT RUN in this planning task.
No C++/CMake/dependency/CI implementation changed, so no new compiler/platform
qualification is claimed. Prior supplemental Linux and Windows sanitizer results
remain historical foundation evidence, not MMORPG qualification.

## Remaining decisions and next task

World type is settled. Combat style, world dimensions/travel speed, final active
player/crowd target, deployment geography/cost and client hardware need refinement
before their owning gates. This does not block documenting the required boundaries.

The immediate next task is to inspect/close assignment 01's actual hosted
cross-platform and deliberate-failure/red-to-green evidence. Then implement
**M01 - Headless server and client/core build separation**, before 02 graphics
feasibility. No hosted run result or branch-protection state was inferred from
the existence of `origin/main`.

Suggested commit text:

```text
docs: retarget Shark for a seamless open-world MMORPG

- define authoritative server, persistence and seamless-world contracts
- insert staged MMO implementation and qualification gates
- assess the existing foundation and retain its working bootstrap
- update agent guidance and ordered requirement validation
```
