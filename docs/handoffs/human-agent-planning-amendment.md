# Human and terminal-agent planning amendment handoff

Date: 2026-10-17 (America/New_York). Scope: user-requested planning amendment and
assessment of the completed foundation code. This does not implement assignment
15, advance the current assignment or qualify a game-authoring API.

## Revision and ownership

Started from clean `main` at `d9f53ef94a6a97a02128d35dc6077a5148467a3e`
(`feat(server): add headless foundation and core/client boundaries`). The user
has committed M01 since its implementation handoff. This amendment is uncommitted;
no resulting commit exists yet. All staging, committing and pushing remain with
the user. No remote CI or branch-protection operation was performed here.

## Decision and assessment

[ADR 0008](../architecture/0008-human-agent-authoring.md) defines independent human
editor and terminal LLM workflows over shared schemas/services. A normal terminal
is the baseline; a provider SDK, embedded LLM or MCP adapter is not required.
The [versioned plan](../roadmap/human-agent-plan.md) contains the foundation audit,
workflow, per-owner gates and three reviewable parts of assignment 15.

Keep the existing C++/CMake/dependency/CI implementation. Core/Client/Tools/Server
separation, noninteractive developer commands, strict bootstrap failures and
bounded JSON/JUnit test evidence already support development by either audience.
There are no existing scene/history/import/editor services to rewrite. Build
tools allow engine-code development now; public game-authoring CLI and editor
workflows are still absent. Bootstrap help/version and Server lifecycle do not
substitute for them. Ordinary same-preset CI runs share output/build paths and
report HEAD; serialize them and record worktree changes explicitly in evidence.

The amendment makes previously broad intentions testable: versioned clean machine
output, capability/schema discovery, bounded semantic queries, explicit jobs and
artifacts, revision-aware edits/text writes, retry-after-lost-reply behavior and
one project owner with safe human/agent handoff. CPU authoring remains separate
from optional GPU preview and from the dedicated Server. Player and live-admin
authority stay outside local content authoring. Actual simultaneous editor/agent
attachment is optional; a tested explicit busy/handoff policy is sufficient.

Two independent audits confirmed the existing foundation and assignment order
remain suitable. Review clarified that save/close/reopen preserves saved content
and resolves dirty state without promising undo history survives restart. Real
Windows/POSIX pipes and Unicode/space-containing paths are explicit 15 fixtures.

## Changes and coverage

- Added ADR 0008, the human/agent plan and this handoff; linked them from README,
  architecture index, AGENTS, build guidance and the MMORPG plan.
- Amended ADRs 0001/0004/0006/0007, 21 existing requirement entries and relevant
  component/import/release skills. Added `REQ-CROSS-HUMAN-AGENT`, owned by 32.
- The manifest now has 50 requirements across the same 46 assignments. Schema 3,
  assignment order, current M01, every original criterion/verification state and
  all implemented-surface entries are unchanged. New acceptance fixtures are
  planned, not registered tests or invented APIs.
- The contract checker now requires the new documents and checks their links.
  No C++ source, target graph, dependency pin or CI workflow changed.
- Updated the ignored local roadmap for consistency. `.gitignore` retains its
  rules and documents `out/` for generated agent/session evidence; public schemas,
  examples, fixtures and skills remain eligible for version control.

No project/source/protocol data format exists to migrate in this amendment.
Historical M01 receipts remain historical evidence of their recorded source and
tests, not qualification of future human/agent authoring behavior.

## Verification

On Windows x64 with the existing Python 3.13.5 tool environment:

| Command/check | Result |
| --- | --- |
| `.tools/venv/Scripts/python.exe tools/validate_contract.py --self-test` | PASS: 50 requirements, 46 ordered owners, links, 20 negative cases and the positive non-lexical-order case |
| `.tools/venv/Scripts/python.exe tools/check_repository.py --self-test` | PASS: five skill sources, metadata/ignore boundaries and three malformed-skill cases |
| `powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only` | PASS: enumerated the existing 15 tests before selecting the contract label |
| Same wrapper with `ctest --preset windows-debug -L contract --output-on-failure --output-junit out/ci/human-agent-contract.xml` | PASS: 2/2, `shark.contract` and `shark.repository`; no skips |
| Python comparison against `git show HEAD:docs/coverage/requirements.json` | PASS: all 49 original requirements, owners, criteria prefixes and verification states retained; exactly 21 strengthened, one planned requirement added; schema/order/current assignment/implemented mappings unchanged |
| `git diff --check`, `git diff --cached --stat` | PASS: no whitespace errors; index empty |
| `git check-ignore -v AI_Game_Engine_Agent_Roadmap.txt out/agent-session/report.json` | PASS: local roadmap and generated evidence ignored; source documents remain visible |

JUnit is retained at `build/windows-debug/out/ci/human-agent-contract.xml` (CTest
resolves the relative output under the preset build directory). C++ compilation,
full native/sanitizer matrices, Linux/macOS, graphics, actual editor/CLI authoring,
fresh LLM session and hosted CI were NOT RUN in this planning task. No engine or
build changes justify repeating their historical tests; these checks establish
document/manifest integrity only, not future product behavior.

## Next task

Close M01's recorded hosted gate on the actual pushed implementation revision,
then proceed to **02 - Graphics compatibility gate**. This task did not inspect
or infer remote run outcomes. 03/07-09 lay structured errors/schema/transaction
foundations; 14/15 exposes the terminal authoring surface; 16/17 proves editor
parity. 33 and M09 supply fresh-agent plus human continuation acceptance.

Suggested commit text:

```text
docs: define shared human and terminal-agent development workflows

- specify noninteractive authoring, discovery and structured results
- define project ownership, revision conflicts and safe human-agent handoff
- extend existing assignment gates and assess the current foundation
```
