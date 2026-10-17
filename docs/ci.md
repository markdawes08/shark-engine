# CPU CI and evidence

Assignment 01's hosted gate is **PASS** on
`3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de`. After repairing the macOS ARM Xcode
selection, the ordinary push passed all fourteen jobs. The subsequent injected
run failed only the sentinel in each of twelve CPU jobs; the restored run passed
all fourteen jobs and all 120 CPU tests on the same revision. See the
[CI verification handoff](handoffs/01-ci-verification.md) and
[durable gate evidence](coverage/evidence/01-ci-gate.json). The user reserves Git
staging, committing and pushing.

The [MMORPG planning amendment](handoffs/mmorpg-planning-amendment.md) retains
assignment 01 as the current gate, followed by M01 before 02.
The existing fourteen required jobs remain the current configuration. M01 must
add genuine headless Server/Core checks on a minimal Ubuntu24.04 environment;
M02-M10 introduce network, database, world-handoff and active-load tiers. Those
jobs/commands are planned and are not provided by the present CPU workflow.

## Jobs and required checks

[The workflow](../.github/workflows/cpu.yml) runs on pushes, pull requests and
manual dispatch. It requests read-only repository permission, checks out without
persisting credentials, and uploads logs on failure as well as success.

| Job names | Runner / architecture | Selected compiler | Configuration |
| --- | --- | --- | --- |
| `quality` | `ubuntu-24.04` x64 | None | Contract/negative checks, workflow syntax and C++ formatting |
| `cpu-windows-debug`, `cpu-windows-release`, `cpu-windows-asan` | `windows-2025-vs2026` x64 | VS 2026, MSVC toolset 14.51, Windows SDK 10.0.26100.0 | Debug, Release, ASan |
| `cpu-linux-debug`, `cpu-linux-release` | `ubuntu-24.04` x64 | GCC 13 (`g++-13`) | Debug, Release |
| `cpu-linux-asan-ubsan` | `ubuntu-24.04` x64 | Clang 18 (`clang++-18`) | ASan + UBSan |
| `cpu-macos-arm64-debug`, `cpu-macos-arm64-release`, `cpu-macos-arm64-asan-ubsan` | `macos-26` arm64 | Xcode 26.4.1 / Apple Clang | Debug, Release, ASan + UBSan |
| `cpu-macos-intel-debug`, `cpu-macos-intel-release`, `cpu-macos-intel-asan-ubsan` | `macos-15-intel` x64 | Xcode 16.4 / Apple Clang | Debug, Release, ASan + UBSan |
| `runtime-only` | `ubuntu-24.04` x64 | GCC 13 | Release, no Editor/EngineTools/Tests |

All fourteen named jobs are configured as required checks on `main`, bound to
the GitHub Actions app (15368), with up-to-date branches required. Administrator
bypass remains enabled for the owner's direct-push workflow; no approving-review
or push-actor restriction was added. The settings were read back from GitHub and
[recorded](coverage/evidence/01-ci-protection.json). Missing, skipped or cancelled
required jobs do not establish a passing assignment gate. The Windows Server
runner is build evidence; it does not qualify Windows 11 graphics or deployment.
The Mac CPU jobs do not qualify either Vulkan-on-Metal implementation.

The matrix checks Debug and Release independently and runs sanitizers on all four
OS/CPU rows. MSVC supplies ASan only; UBSan is exercised by Clang and Apple Clang.
Runtime-only has no CTest preset because Tests are disabled. That job explicitly
builds the boundary target and runs Runtime's real `--version` behavior.

## Locked tools and hosted-image limits

[requirements-dev.txt](../requirements-dev.txt) locks these distributions with
SHA-256 hashes for Windows x64, Linux x64 and both macOS CPU architectures:
CMake 3.31.10, Ninja 1.13.2, clang-format 20.1.8 and PyYAML 6.0.3. Python is exactly
3.13.5. Install only wheels with the documented hash-checking command in
[building.md](building.md). The Ninja wheel's executable identifies its vendor
build as `1.13.2.git.kitware.jobserver-pipe-1`; the wheel version remains 1.13.2.
The exact executable banner and hash are retained in evidence. PyYAML is only a
development workflow validator and never enters Engine or Runtime.

The action revisions are immutable commit pins, verified against their upstream
release tags on 2026-10-17:

- `actions/checkout`: `08c6903cd8c0fde910a37f88322edcfb5dd907a8`
  ([v5.0.0](https://github.com/actions/checkout/releases/tag/v5.0.0)).
- `actions/setup-python`: `e797f83bcb11b83ae66e0230d6156d7c80228e7c`
  ([v6.0.0](https://github.com/actions/setup-python/releases/tag/v6.0.0)).
- `actions/upload-artifact`: `ea165f8d65b6e75b540449e92b4886f43607fa02`
  ([v4.6.2](https://github.com/actions/upload-artifact/releases/tag/v4.6.2)).

Hosted runner OS labels select a platform, not an immutable VM snapshot. GitHub
updates these images regularly. The workflow explicitly selects an installed
Xcode bundle, MSVC toolset family/SDK and GCC/Clang executable family; it fails
when a requested bundle is absent instead of silently choosing another Xcode or
Visual Studio major version. Full compiler patch/build versions, compiler binary
hashes, SDK selection and `ImageOS`/`ImageVersion` are recorded after configure.
Upstream inventory documents the selected labels and installed tools; actual
results and recorded images are linked in the handoff.
[GitHub runner image policy and inventory](https://github.com/actions/runner-images)

The ARM64 Xcode pin is 26.4.1 because Apple documents sanitizer hangs on macOS
26.4 with binaries built by Xcode 26.3 or older (issue 171762808), and recommends
Xcode 26.4. The observed failures used Xcode 26.0.1 on macOS 26.6.2 and match
the reported issue. Native CI now passes with Xcode 26.4.1 on the same OS/image.
All three ARM configurations select the same updated bundle. Sanitizers and
existing test deadlines remain enabled. [Apple Xcode 26.4 release notes](https://developer.apple.com/documentation/xcode-release-notes/xcode-26_4-release-notes)

Qualification amendment to ADR 0002: exact project tools and source dependencies
are immutable inputs; hosted CI compiler families/bundles are explicitly selected
and their exact observed binaries recorded per run. This is a controlled rolling
CI configuration, not a claim that a hosted VM can be recreated from its label.
Pinning the action or Python version does not freeze the OS, standard library,
Apple SDK, compiler patch or Python distributor rebuild. Assignment 35 must archive
the actual toolchain/environment and build inputs needed for release reproduction,
or use an immutable controlled runner image. A missing bundle or changed compiler
requires a reviewed configuration/evidence update; do not mask that failure.

## Shared local and CI command

After installing the locked tools and entering the appropriate compiler environment:

```text
python tools/ci.py --preset windows-debug
python tools/ci.py --preset windows-release
python tools/ci.py --preset windows-asan
python tools/ci.py --preset linux-debug
python tools/ci.py --preset linux-release
python tools/ci.py --preset linux-asan-ubsan
python tools/ci.py --preset macos-debug
python tools/ci.py --preset macos-release
python tools/ci.py --preset macos-asan-ubsan
```

Run only the presets for the current OS. Windows requires the native x64 MSVC
developer environment; a plain shell with MSYS CMake earlier on PATH is not a
valid substitute. The helper verifies the Python and tool pins before configuring.
An explicit `CXX` environment value overrides the preset's portable compiler name;
CI uses this to select the versioned GCC/Clang binary or Xcode's compiler.

The helper configures with `SHARK_INJECT_TEST_FAILURE=OFF` unless explicitly
requested, builds with two parallel jobs, and runs CTest with `--no-tests=error`
and JUnit output. `--jobs N` changes parallelism; `--timeout N` sets a positive
per-subprocess timeout in seconds, default 1200. A configure/build/test failure
stops later phases and returns a nonzero exit status. There is no expected-failure
inversion or failure-swallowing path. Tool-version probes have a 30-second timeout.
The shared process runner owns a Windows job/process tree or a private Unix
session. Timeout terminates that tree, returns 124 and allows at most three extra
one-second cleanup waits on Windows or one on Unix. Output is captured into
temporary files, so inherited pipe handles cannot extend the deadline indefinitely.
Background descendants are cleaned up even after normal parent exit. Commands
must not deliberately escape their Unix session. Process creation and OS file
operations themselves cannot be interrupted by Python's subprocess deadline.

Evidence is written under `out/ci/<preset>/`: `summary.json`, `toolchain.json`,
phase logs and `ctest.xml` when CTest ran. The summary lists exact commands and
exit codes; an unexecuted phase has no result and is not a PASS. Previous phase
logs and JUnit are removed at invocation start to avoid stale successful evidence.
Only a fixed allowlist of non-secret environment metadata is recorded. Local
revision capture reports a missing Git repository honestly. Artifacts are uploaded
with distinct platform/architecture names and retained for 30 days; copy required
acceptance evidence to durable release/handoff storage before expiration.

## Repeating the red/green proof

The assignment 01 proof is complete: [injected run 39077652643](https://github.com/markdawes08/shark-engine/actions/runs/39077652643)
followed by [restored run 39078070735](https://github.com/markdawes08/shark-engine/actions/runs/39078070735),
both at `3620fab`. Use this procedure when a future build/CI change requires
renewed failure-propagation evidence. Finish the first dispatch before starting
the second; the workflow's concurrency policy can cancel overlapping dispatches.

1. Open the repository's **Actions > CPU foundation > Run workflow** on the
   intended revision. Set `inject_test_failure` to **true**. The twelve CPU jobs
   must reach CTest, execute the deliberately failing test and fail. A tool-install,
   configure or compilation failure does not prove test failure propagation.
2. Save the run URL, commit, runner metadata, each CPU job's nonzero test-phase
   exit code and JUnit containing the injected test failure. `quality` and
   `runtime-only` may remain green; the overall workflow must be red.
3. Dispatch the same revision again with `inject_test_failure` **false** (the
   default). All fourteen jobs must pass, and no injected failure may remain in
   CTest. Save this run URL and artifacts alongside the failed-run evidence.
4. Update the assignment 01 handoff and requirement status only after checking
   both actual runs and the four native CPU rows. Ordinary failed push runs do
   not prove deliberate test failure propagation. Until the injected failure
   and restored pass are verified on the same revision, a new red/green claim
   remains unverified. Keep the previously verified revision explicit.

The same real failure can be checked locally, for example in a Windows developer
shell: `python tools/ci.py --preset windows-debug --inject-test-failure` must fail;
then `python tools/ci.py --preset windows-debug` must pass. The second invocation
explicitly resets the cached option to OFF. Preserve the first output directory
before rerunning if both evidence sets are needed. Local proof does not replace
the required hosted-job result.

## Configuration verification

`python tools/check_ci_config.py` parses the YAML and checks matrix coverage,
immutable action references, timeouts, least-privilege checkout wiring and the
manual failure input. It does not execute GitHub expression evaluation or jobs.
`python tools/check_format.py` checks engine-owned C/C++ files against the pinned
formatter; `--fix` deliberately applies formatting. No future GPU, import or
release tests are invented for this foundation.
