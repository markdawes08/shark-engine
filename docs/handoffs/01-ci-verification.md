# Assignment 01 CI verification follow-up

Date: 2026-10-17. Assignment: **01 - Repository, build system and test foundation**.
Full gate: **PASS**. This follows the [implementation handoff](01-build-foundation.md)
and [MMORPG planning amendment](mmorpg-planning-amendment.md).

## Revision and scope

Base: `7aac0b120666de0c0368e3500fe5630165c2e5a5` on `main`, with a clean worktree
at investigation start. Remote: `markdawes08/shark-engine`. The user committed
and pushed the repair as `3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de`; all three
successful verification stages below tested that revision. The completion
documents/evidence are a subsequent uncommitted update. The user retains all
staging, committing and pushing.
No C++ behavior, public API, dependency source pin or target boundary changed.

The repair commit changed `.github/workflows/cpu.yml` to Xcode 26.4.1 for the three
macOS ARM configurations and recorded the initial investigation. This completion
updates AGENTS, README, CI/coverage documentation, this handoff, the MMORPG plan
and status references in ADRs 0001/0003, and adds four durable evidence reports.
REQ-01 is now PASS; current assignment remains 01. Feature mappings and
FeatureWorld coverage are unchanged. `.gitignore` was reviewed: `/out/` already
covers downloaded raw evidence and the temporary verifier, and the local roadmap
remains ignored. The reports under `docs/coverage/evidence/` intentionally preserve
acceptance findings in version control.

## Initial hosted failures

These are ordinary push runs with failure injection disabled, not the required
deliberate-failure proof:

| Run | Commit | Result |
| --- | --- | --- |
| [39075352115, attempt 1](https://github.com/markdawes08/shark-engine/actions/runs/39075352115) | `3fecfc3cc96b6f385a912a9b3737c75e1e19aa8e` | FAIL: 13/14 jobs pass; ARM64 ASan/UBSan fails |
| [39076666000, attempt 1](https://github.com/markdawes08/shark-engine/actions/runs/39076666000) | `7aac0b120666de0c0368e3500fe5630165c2e5a5` | FAIL: same job and failure pattern |

Downloaded all thirteen artifacts from the second run and inspected each CPU
`summary.json` and JUnit result:

| Hosted row | Debug | Release | Sanitizers |
| --- | --- | --- | --- |
| Windows x64, `windows-2025-vs2026` | 10/10 PASS | 10/10 PASS | ASan: 10/10 PASS |
| Linux x64, `ubuntu-24.04` | 10/10 PASS | 10/10 PASS | ASan/UBSan: 10/10 PASS |
| macOS ARM64, `macos-26`, Xcode 26.0.1 | 10/10 PASS | 10/10 PASS | ASan/UBSan: 6/10 PASS; 4 FAIL |
| macOS Intel, `macos-15-intel`, Xcode 16.4 | 10/10 PASS | 10/10 PASS | ASan/UBSan: 10/10 PASS |

`quality` and `runtime-only` also passed in both runs. Every successful CPU job
recorded configure/build/test exits 0/0/0 and `injected_failure: false`.
The failing job recorded exits 0/0/8. Unit and failure-sentinel tests timed out
at ten seconds; Runtime and Editor CLI checks timed out on their first version
command at five seconds. The other six CTests passed. This is not an injected
assertion failure, compile failure or the outer CI helper's 1,200-second timeout.

Failed [job 117525120417](https://github.com/markdawes08/shark-engine/actions/runs/39076666000/job/117525120417)
recorded macOS 26.6.2 (25G83), Darwin 25.6.0, ARM64, image
`20261013.0424.1`, Xcode 26.0.1 (17A400), Apple Clang `17.0.0.17000319` and
SDK 26.0. Python 3.13.5 and locked CMake/Ninja/format/PyYAML packages matched.

Commands used to retrieve evidence:

```text
gh run view 39075352115 --repo markdawes08/shark-engine --json status,conclusion,headSha,url,jobs
gh run view 39076666000 --repo markdawes08/shark-engine --json status,conclusion,headSha,url,jobs
gh run download 39075352115 --repo markdawes08/shark-engine --name cpu-macos-arm64-asan-ubsan-1 --dir out/ci/hosted/39075352115/macos-arm64-asan-ubsan
gh run download 39076666000 --repo markdawes08/shark-engine --dir out/ci/hosted/39076666000
gh run view 39076666000 --repo markdawes08/shark-engine --job 117525120417 --log
```

Downloaded logs, JUnit and toolchain reports are retained locally under the
ignored `out/ci/hosted/` tree. GitHub artifacts expire after 30 days; this tracked
handoff preserves the initial findings, and the reports below preserve the final
acceptance evidence, including assertions, commands, toolchains and file hashes.

## Repair and local checks

Apple documents ASan/TSan hangs on macOS 26.4 when compiled using Xcode 26.3 or
older (issue 171762808), with Xcode 26.4 as the workaround. The observed toolchain
and symptoms match that issue; there is no native stack sample proving the
specific hang location. [Apple release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_4-release-notes)

The workflow now selects `/Applications/Xcode_26.4.1.app/Contents/Developer`
for all three ARM jobs, keeping Debug/Release/sanitizers on one toolchain.
The [exact observed runner inventory](https://github.com/actions/runner-images/blob/macos-26-arm64/20261013.0424/images/macos/macos-26-arm64-Readme.md)
lists this versioned bundle, build 17E202. ASan/UBSan, warnings-as-errors, all ten tests and
existing deadlines remain enabled. Intel Mac retains its separate Xcode 16.4
selection. No runtime-library substitution or failure suppression was added.

Local verification used Windows 11 x64 and the existing pinned Python 3.13.5
environment. Commands from the repository root:

| Command | Result |
| --- | --- |
| `.tools/venv/Scripts/python.exe tools/check_ci_config.py` | PASS: workflow structure, matrix, action pins and failure wiring |
| `.tools/venv/Scripts/python.exe tools/validate_contract.py --self-test` | PASS: 49 requirements, 46 ordered assignments, links, 20 negative checks and insertion-order positive case |
| `.tools/venv/Scripts/python.exe tools/check_repository.py --self-test` | PASS: five skills, ignore boundaries and three negative metadata cases |
| `powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/check_format.py` | PASS: all seven owned C++ files |
| `git diff --check` | PASS |

No local C++ rebuild was needed for this workflow/documentation-only change.
Native execution of the new Xcode pin is verified in the hosted runs below. Independent
review of the existing process runner, Catch2 setup and sanitizer flags found
no concrete defect explaining these macOS-only timeouts.

## Completed hosted gate

All runs below used `3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de`, attempt 1:

| Run | Input | Verified result |
| --- | --- | --- |
| [39077290171](https://github.com/markdawes08/shark-engine/actions/runs/39077290171) | Ordinary push, injection OFF | PASS: 14/14 jobs; 120/120 CPU tests |
| [39077652643](https://github.com/markdawes08/shark-engine/actions/runs/39077652643) | Manual dispatch, injection ON | EXPECTED FAIL: all 12 CPU jobs fail only `shark.failure-sentinel`; other 108 tests pass; quality/runtime-only pass |
| [39078070735](https://github.com/markdawes08/shark-engine/actions/runs/39078070735) | Later manual dispatch, injection OFF | PASS: 14/14 jobs; 120/120 CPU tests; no skips or remaining injected assertion |

Every CPU job reached all three phases. Injected phase exits were 0/0/8 and each
JUnit recorded the real `REQUIRE(1 == 0)` assertion with its sentinel diagnostic.
Restored phase exits were 0/0/0. Actual test names, assertion output, flags,
compiler/image identity, checkout SHA, run attempt and selected workflow steps
were inspected in the downloaded artifacts and GitHub API responses. The
injected run finished before the restored run started. No jobs were cancelled,
retried or accepted merely because they failed.

The final run recorded these native toolchains; every row exercised Debug,
Release and its sanitizer configuration:

| Runner / CPU | Observed compiler / SDK | Image version |
| --- | --- | --- |
| `windows-2025-vs2026` x64 | MSVC 19.51.36260.0; Windows SDK 10.0.26100.0; ASan | `20261007.264.1` |
| `ubuntu-24.04` x64 | GCC 13.3.0; Clang 18.1.3 for ASan/UBSan | `20261012.334.1` |
| `macos-26` ARM64 | Xcode 26.4.1 (17E202), Apple Clang 21.0.0.21000099, SDK 26.4; ASan/UBSan | `20261013.0424.1` |
| `macos-15-intel` x64 | Xcode 16.4 (16F6), Apple Clang 17.0.0.17000013, SDK 15.5; ASan/UBSan | `20260824.0482.1` |

All retained Python/development-tool pins matched. These are managed CPU runners;
no graphics or audio devices were exercised. Sanitizer selection was verified
from recorded configure data and reviewed build sources, without a separate
binary-instrumentation audit.

Dispatch commands, with `main` checked against the tested SHA before each:

```text
gh workflow run cpu.yml --repo markdawes08/shark-engine --ref main -f inject_test_failure=true
gh workflow run cpu.yml --repo markdawes08/shark-engine --ref main -f inject_test_failure=false
```

An ignored one-off verifier checked API data and downloaded artifacts, using:

```text
.tools/venv/Scripts/python.exe -B out/ci/hosted/verify_run.py --run-id 39077290171 --expected-sha 3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de --mode normal
.tools/venv/Scripts/python.exe -B out/ci/hosted/verify_run.py --run-id 39077652643 --expected-sha 3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de --mode injected
.tools/venv/Scripts/python.exe -B out/ci/hosted/verify_run.py --run-id 39078070735 --expected-sha 3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de --mode normal
```

All returned verification PASS; the injected workflow itself correctly remained
red. The helper is local investigation tooling, not a new checkout prerequisite.
Its parsing checks rejected the original timeout evidence as an injected-test
proof. Same-revision ordering and the live required-check list were additionally
verified across reports before marking the gate complete.

Durable reports:

- [Gate and report hashes](../coverage/evidence/01-ci-gate.json).
- [Injected run](../coverage/evidence/01-ci-red.json), including actual failed assertions.
- [Restored run](../coverage/evidence/01-ci-green.json), including phase commands,
  test totals, exact compilers/SDKs/images, artifact identities and extracted-file hashes.
- [Required-check configuration](../coverage/evidence/01-ci-protection.json).

Artifact archive digests are those returned by GitHub; extracted-file SHA-256
values were independently computed. These reports preserve acceptance findings;
the complete raw logs remain local and in the expiring GitHub artifacts.

`main` had no protection at investigation start. All fourteen documented checks
are now required from GitHub Actions app 15368, with strict branch freshness.
Administrator bypass remains enabled to preserve the owner's direct pushes;
no required approving review or push-actor restriction was introduced. The rule
was applied with `gh api --method PUT repos/markdawes08/shark-engine/branches/main/protection
--input out/ci/hosted/main-protection-request.json` and verified through a subsequent
GET of the same endpoint. Force pushes and branch deletion remain disallowed.

## Next assignment and limits

Assignment 01 has no remaining acceptance blocker. The exact next assignment is
**M01 - Headless server and
client/core build separation**, before 02 graphics feasibility. No MMO, graphics,
audio-device or release-package qualification is claimed by these CPU results.

Suggested commit text:

```text
docs: close assignment 01 hosted CI gate

- archive verified failure/restoration runs and required-check settings
- mark the CPU foundation gate passed and M01 ready to start
```
