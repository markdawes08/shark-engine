# Assignment 01 CI verification follow-up

Date: 2026-10-17. Assignment: **01 - Repository, build system and test foundation**.
Full gate: **FAIL / OPEN**. This follows the [implementation handoff](01-build-foundation.md)
and [MMORPG planning amendment](mmorpg-planning-amendment.md).

## Revision and scope

Base: `7aac0b120666de0c0368e3500fe5630165c2e5a5` on `main`, with a clean worktree
at investigation start. Remote: `markdawes08/shark-engine`. The result is an
uncommitted local repair; the user retains all staging, committing and pushing.
No C++ behavior, public API, dependency source pin or target boundary changed.

Changed files: `.github/workflows/cpu.yml` selects Xcode 26.4.1 for the three
macOS ARM configurations; `docs/ci.md`, this handoff, the historical handoff link,
README and `docs/coverage/` record actual evidence. REQ-01 is
FAIL instead of NOT_RUN; current assignment remains 01. Feature mappings and
FeatureWorld coverage are unchanged. `.gitignore` was reviewed: `/out/` already
covers downloaded evidence, and the local roadmap remains ignored.

## Observed hosted results

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
handoff preserves the findings, not the full logs.

## Repair and verification limits

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
Native execution of the new Xcode pin is **NOT RUN** because this workspace is
Windows and hosted Actions requires the user to publish the change. Independent
review of the existing process runner, Catch2 setup and sanitizer flags found
no concrete defect explaining these macOS-only timeouts.

## Remaining gate and next assignment

No manual workflow has been dispatched in this follow-up. `main` currently has
no branch protection (GitHub API reports 404); repository administration is
available. Required checks remain to be configured after validating the repair.

1. The user commits/pushes the repair; verify the ordinary run, including ARM64
   ASan/UBSan, passes on that exact revision.
2. Dispatch `cpu.yml` on that revision with `inject_test_failure=true`. All twelve
   CPU jobs must reach CTest and fail specifically at `shark.failure-sentinel`,
   with the other nine tests passing. Preserve run URL, SHA, artifacts and exits.
3. After that run completes, dispatch on the same revision with
   `inject_test_failure=false`; all fourteen jobs must pass. Running these
   dispatches concurrently can cancel the first due to workflow concurrency.
4. Record both runs and configure the fourteen documented required checks.
   Update REQ-01 only after the actual acceptance evidence is verified.

After this gate passes, the exact next assignment is **M01 - Headless server and
client/core build separation**, before 02 graphics feasibility. No MMO, graphics,
audio-device or release-package qualification is claimed by these CPU results.

Suggested commit text:

```text
fix(ci): update macOS ARM Xcode to avoid sanitizer hangs

- pin all macOS ARM builds to Xcode 26.4.1
- record hosted results and the outstanding red/green CI gate
```
