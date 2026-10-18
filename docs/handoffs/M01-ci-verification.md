# M01 hosted CI verification follow-up

Date: 2026-10-18 UTC (2026-10-17 America/New_York). Assignment: **M01 - Headless
server and Core/Client separation**. Full gate: **PASS**. This completes the
[implementation handoff](M01-headless-server.md); assignment 02 has not started.

## Revision and scope

Started from clean `main` at `d658d69c21c492c844f5678f931b1ddf955d25cd` in
`markdawes08/shark-engine`. The user committed/pushed M01 as
`d9f53ef94a6a97a02128d35dc6077a5148467a3e`, followed by the human/agent planning
amendment at `d658d69`. The normal, deliberate-failure and recovery runs below
all qualify the latter revision. Completion documentation/evidence is a subsequent
uncommitted update. The user retains all staging, committing and pushing.

No C++ behavior, public API, dependency input, target graph or workflow needed
repair. The only remote change adds the verified headless check to the existing
branch policy. Local changes record that evidence, require this handoff in the
document validator, mark REQ-M01 PASS and update current-status references.
Current assignment remains M01, complete; implemented-surface mappings are unchanged.
`.gitignore` already covers raw downloads/audit helpers under `out/`; durable
reports under `docs/coverage/evidence/` intentionally remain visible.

## Completed hosted evidence

All runs are attempt 1; no retry or cancellation was used to obtain acceptance.

| Run | Revision / input | Verified result |
| --- | --- | --- |
| [39081460198](https://github.com/markdawes08/shark-engine/actions/runs/39081460198) | `d9f53ef`, original M01 push | PASS: 15/15 jobs; original headless artifact independently audited |
| [39082339515](https://github.com/markdawes08/shark-engine/actions/runs/39082339515) | `d658d69`, ordinary push, injection OFF | PASS: 15/15 jobs, 206/206 CTests |
| [39082507455](https://github.com/markdawes08/shark-engine/actions/runs/39082507455) | `d658d69`, manual dispatch, injection ON | EXPECTED FAIL: 12 CPU jobs and headless-server fail only `shark.failure-sentinel`; other 180 tests pass |
| [39083098689](https://github.com/markdawes08/shark-engine/actions/runs/39083098689) | `d658d69`, later manual dispatch, injection OFF | PASS: 15/15 jobs, 206/206 CTests, no skips or disabled tests |

Each CPU job runs 15 tests. Headless runs 13 Debug and 13 ASan/UBSan tests, for
180 + 26 = 206 in a complete normal run. The injected run reaches 193 tests with
exactly 13 sentinel failures and no skips. CPU phases exit 0/0/8 and each JUnit
contains the actual `1 == 0` assertion. Headless Release/package phases keep
injection OFF; Debug reaches its sentinel and returns 8. The helper then stops,
so its later sanitizer and pristine-runtime stages are NOT_RUN in that red run.
Quality and runtime-only pass. No setup/build failure substitutes for this proof.

The injected workflow completed before recovery was dispatched. Recovery started
at `2026-10-18T00:31:54Z`; its final job completed at `00:37:31Z`. All ordinary
CPU phase exits are restored to 0/0/0, all fourteen headless phases pass and the
pristine-runtime smoke succeeds. API job identities, exact SHA/attempt, native
runner architecture, toolchain/configuration, phase commands and exact JUnit test
names were checked, not merely the workflow's green badge.

| Native row | Observed compiler / SDK | Runner image | Restored tests |
| --- | --- | --- | --- |
| Windows x64 | MSVC 19.51.36260.0, SDK 10.0.26100.0; ASan | `20261007.264.1` | 15 Debug + 15 Release + 15 ASan |
| Ubuntu x64 | GCC 13.3.0; Clang 18.1.3 for ASan/UBSan | `20261012.334.1` | 15 + 15 + 15 |
| macOS ARM64 | Xcode 26.4.1, Apple Clang 21.0.0.21000099, SDK 26.4 | `20261013.0424.1` | 15 + 15 + 15 |
| macOS Intel | Xcode 16.4, Apple Clang 17.0.0.17000013, SDK 15.5 | `20260824.0482.1` | 15 + 15 + 15 |

Desktop Python 3.13.5 and all locked tool versions match. The sanitizer evidence
checks actual tests and recorded compiler/configuration; it is not a separate
binary-instrumentation audit. Windows finite-lifetime tests pass; POSIX signal
and read-only-descriptor cases run on Linux/macOS rather than being simulated
with Windows process termination.

## Hosted headless and package evidence

The actual Docker job uses the pinned Ubuntu 24.04 amd64 base and signed snapshot
`20261017T000000Z`. Its 187 installed package/version/architecture rows exactly
match the lock as an inventory; TSV row order is not package identity. GCC 13.3.0,
Clang 18.1.3, glibc 2.39 and the explicit Python 3.12.3 exception remain selected.
The restored container sees AMD EPYC 7763, four logical CPUs, 16,373,452 kB memory
and kernel `6.17.0-1022-azure`; this is a shared hosted kernel, not bare-metal
performance or capacity qualification. Source Git SHA is verified and clean.

Release configure/build/boundary/install/closure/smoke pass. The closure contains
only EngineCore, Server and the boundary guard, with Client/Editor/Tests OFF and
FetchContent fully disconnected. It audits 324 compiler-discovered inputs plus
actual archive/link/package data. The package contains only `bin/Server`, whose
SHA-256 is `405b5b069fe3ab56e38c26c96ee9e4e6d49176410e512a9292439efaf06cf213`.
Direct ELF dependencies are libstdc++, libgcc_s and libc; resolved dependencies
add only libm and the ELF loader. Original, restored and historical local package
hashes agree.

GCC Debug and Clang ASan/UBSan each pass 13/13 tests with leak detection and
halt-on-error enabled. The lifecycle fixture covers 24 invalid configurations,
six finite lifetimes, ready-process SIGINT/SIGTERM, maximum-duration interruption
and readiness-write failure cleanup. A separate pristine base with 92 packages
and no compiler/Python/presentation dependencies starts the installed Server for
20 ms. The exact tested wrapper uses `--network none --read-only` and a read-only
package mount. Successful job/log artifacts establish that smoke; runtime
container inspection is not retained. Full lifecycle tests run in the build/test
container, not in this pristine smoke. License/input inventories are retained;
this executable-only fixture is not a licensed shipping game/server distribution.

The earlier local WSL route used private chroots and a writable runtime root.
Hosted Docker now qualifies the separate read-only-root route without changing
what those historical local receipts prove.

## Retrieval, audit and durable records

Remote `main` was checked against the full SHA before each dispatch; resulting
run metadata was checked afterward. Executed commands include:

```text
gh workflow run cpu.yml --repo markdawes08/shark-engine --ref main -f inject_test_failure=true
gh workflow run cpu.yml --repo markdawes08/shark-engine --ref main -f inject_test_failure=false
gh run view 39083098689 --repo markdawes08/shark-engine --json status,conclusion,jobs
.tools/venv/Scripts/python.exe out/ci/m01-hosted/audit_run.py --run 39082339515 --revision d658d69c21c492c844f5678f931b1ddf955d25cd --mode green --output out/ci/m01-hosted/push
.tools/venv/Scripts/python.exe out/ci/m01-hosted/audit_run.py --run 39082507455 --revision d658d69c21c492c844f5678f931b1ddf955d25cd --mode red --output out/ci/m01-hosted/red-verified
.tools/venv/Scripts/python.exe out/ci/m01-hosted/audit_run.py --run 39083098689 --revision d658d69c21c492c844f5678f931b1ddf955d25cd --mode green --output out/ci/m01-hosted/green
```

The one-off ignored auditor retrieves run/jobs/artifact metadata, every job log
and all fourteen artifact ZIPs per run through `gh api`. Downloaded ZIP bytes
match GitHub SHA-256 digests; extracted files are also hashed. It checks exact
test selections, phase exits, injection/cache flags, source revision and native
rows. The initial red audit mistakenly classified CTest's expected `status="fail"`
as skipped; the parser was corrected to require that status only for the injected
sentinel, while retaining zero skipped/disabled/error requirements. Original CI
artifacts were unchanged; the failed audit is retained under `out/ci/m01-hosted/red/`.

The [gate receipt](../coverage/evidence/M01-ci-gate.json) links hashes of the
[normal push](../coverage/evidence/M01-ci-push.json),
[injected run](../coverage/evidence/M01-ci-red.json),
[restored run](../coverage/evidence/M01-ci-green.json),
[protection readback](../coverage/evidence/M01-ci-protection.json) and historical
[local receipt](../coverage/evidence/M01-local.json). These preserve findings,
commands, toolchains, assertions and artifact identities after GitHub's 30-day
artifact retention. Raw ZIPs/logs/audit helpers remain under ignored `out/`.

## Required checks and local completion validation

After recovery passed, a targeted PATCH to
`repos/markdawes08/shark-engine/branches/main/protection/required_status_checks`
added `headless-server` to the existing fourteen checks. The exact request and
before/after API readback are in the protection receipt. All fifteen checks are
bound to GitHub Actions app 15368 with strict branch freshness. Every unrelated
protection field is unchanged: administrator bypass is retained, no review or
push-actor restriction was introduced, and force pushes/deletion remain disabled.

Local completion checks pass on Windows x64 with the pinned Python 3.13.5:

| Command / check | Result |
| --- | --- |
| `.tools/venv/Scripts/python.exe tools/validate_contract.py --self-test` | PASS: 50 requirements, 46 owners, resolved links, 20 negative cases and the positive MMO assignment-order case |
| `.tools/venv/Scripts/python.exe tools/check_repository.py --self-test` | PASS: five repository skills, ignore boundaries and three malformed-skill cases |
| `.tools/venv/Scripts/python.exe tools/check_ci_config.py` | PASS: required CPU and headless job wiring |
| `powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only` | Enumerated all 15 tests before filtering |
| `powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -L contract --output-on-failure --output-junit out/ci/m01-hosted-docs.xml` | PASS: 2/2 contract/repository tests, no skips |
| Gate receipt integrity and manifest comparison to HEAD | All five linked evidence hashes match; only REQ-M01 verification changes, with other requirements and mappings preserved |
| `git diff --check` and `git diff --cached --stat` | PASS: no whitespace errors and no staged changes |

Local JUnit is retained at
`build/windows-debug/out/ci/m01-hosted-docs.xml`. No local C++ rebuild is needed
for these documentation/evidence updates. No stage, commit, push, PR comment or
external message was performed.

## Next assignment and limits

Next is **02 - graphics feasibility**, starting with the agreed **02a dependency
and probe setup** checkpoint. This verification task does not begin it. All nine
real-GPU qualification rows and both native Mac graphics routes remain untested.
Networking, simulation, persistence, streaming, MMO capacity, editor UI and public
game-authoring tools remain future work. M01 closes a real lifecycle/dependency
foundation; it does not qualify a shipping MMORPG.

Suggested commit: `docs: close M01 hosted CI and headless server gate`
