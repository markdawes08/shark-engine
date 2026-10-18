# CPU and headless Server CI

Under [ADR 0010](architecture/0010-current-machine-scope.md), roadmap acceptance
uses this PC's native Windows and existing WSL Ubuntu checks. The nine-job hosted
workflow is retained as supplemental regression coverage; hosted availability is
not a prerequisite for local assignment progression. Existing remote branch
protection still governs GitHub merges and is unchanged. Do not label an absent
or failing hosted run PASS, and investigate real failures affecting the local
targets. No hosted run is required for this planning-only scope amendment.

Assignment 01's hosted gate is **PASS** on
`3620fab38a999a6dd6b6fdbb8c8a25b9a680b6de`. After repairing the macOS ARM Xcode
selection, the ordinary push passed all fourteen jobs. The subsequent injected
run failed only the sentinel in each of twelve CPU jobs; the restored run passed
all fourteen jobs and all 120 CPU tests on the same revision. See the
[CI verification handoff](handoffs/01-ci-verification.md) and
[durable gate evidence](coverage/evidence/01-ci-gate.json). The user reserves Git
staging, committing and pushing.

**M01's hosted gate is PASS** at
`d658d69c21c492c844f5678f931b1ddf955d25cd`. The ordinary push and restored manual
run pass all fifteen jobs and all 206 CTests. The intervening injected run reaches
193 tests, failing only the thirteen intended sentinels while the other 180 pass;
headless sanitizer and clean-runtime stages stop after its Debug failure.
The [M01 hosted follow-up](handoffs/M01-ci-verification.md) and
[durable gate evidence](coverage/evidence/M01-ci-gate.json) record actual runs,
Docker/package qualification and the fifteen checks required at that revision. The
[MMORPG execution plan](roadmap/mmorpg-plan.md) records the completed 04c and full
assignment-04 local gate, following local 03/04a/04b qualification.
05a device ownership is complete locally. **05b presentation lifecycle is
complete under the current-machine scope.** The
[05b handoff](handoffs/05b-presentation-lifecycle.md) records 236 passing CTests,
2,304 validated presentations across both local GPUs, application and protected
package checks. **05c failure and stress qualification** is next.
Its [fixtures and package tools](presentation-qualification.md) are now implemented,
but the [05c native GDI-retention investigation](handoffs/05c-presentation-qualification.md)
leaves the sustained-resource gate open. CPU and short native passes do not replace it.
The [05a handoff](handoffs/05a-graphics-device.md) records 211 CTests and both-GPU
device qualification; full `REQ-05` remains NOT_RUN until 05c. The [opt-in probe](graphics-probe.md) adds CPU checks to
ordinary CI while graphics builds/device runs use explicit graphics presets.
The historical job/test counts above remain tied to their recorded revisions.
[ADR 0009](architecture/0009-windows-linux-platform-scope.md) removes Mac from
the active product and CI scope, retiring 02c and removing the six Mac CPU jobs.
The current workflow has nine jobs, described below. At `39110d4`,
[run 39092472102](https://github.com/markdawes08/shark-engine/actions/runs/39092472102)
passed all nine jobs and 156/156 CTests; the
[02d audit](coverage/evidence/02d-hosted-ci.json) preserves artifact digests,
toolchains, boundary results and protection readback. This verifies the platform
scope amendment, not the later Linux package and link changes committed at
`2ef281b`. No new hosted execution of that revision is claimed here. Windows
relocation and SDK-free WSL package checks meet the amended local acceptance
scope; they remain distinct from independent-machine or native Linux GPU evidence.
The CPU package rejection suite
is registered as `shark.tooling.graphics-package`, with the Linux package and
process-cleanup suite in `shark.tooling.graphics-package-linux`; Windows launcher integration and
GPU soak remain opt-in. Network, database, world-handoff and
active-load tiers remain planned.

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
| `runtime-only` | `ubuntu-24.04` x64 | GCC 13 | Client Release, no Server/Editor/EngineTools/Tests |
| `headless-server` | `ubuntu-24.04` x64 host; locked Ubuntu 24.04 amd64 image | Snapshot GCC 13 and Clang 18 | Server-only Release package, Core/Server Debug tests, ASan + UBSan tests and clean-runtime smoke |

All nine jobs, including `headless-server`, are configured as required checks on
`main`, bound to GitHub Actions app 15368, with up-to-date branches required.
The [scope amendment handoff](handoffs/windows-linux-scope-amendment.md) and
[protection readback](coverage/evidence/windows-linux-ci-protection.json) record
removal of only the six Mac check names. Administrator bypass and all other
protection settings are unchanged. No approving-review or push-actor restriction
was added. The revised workflow passed at `39110d4`; a later revision needs its
own actual run before a hosted PASS can be claimed. This is separate from local
roadmap completion under ADR 0010.
The historical [M01 gate evidence](coverage/evidence/M01-ci-gate.json) and
[01 protection report](coverage/evidence/01-ci-protection.json) retain their
original fifteen- and fourteen-check policies. Missing, skipped or cancelled required jobs
do not establish a hosted PASS or satisfy those remote required checks. The Windows Server
runner is build evidence; it does not qualify Windows 11 graphics or deployment.
The matrix checks Debug and Release independently and runs sanitizers on both
OS/CPU rows. MSVC supplies ASan only; UBSan is exercised by Linux Clang.
Runtime-only has no CTest preset because Tests are disabled. That job explicitly
builds the client boundary target and runs Runtime's real `--version` behavior.
Ordinary CPU presets now also build Server and run its unit/lifecycle checks.
04b adds the [window contract](window-lifecycle.md) and display-free window/CLI
fixtures. Windows client builds compile the pinned GLFW backend but ordinary
CTest never opens a window; explicit native smoke runs qualify the current PC.
Linux CPU and isolated Server builds leave `SHARK_NATIVE_WINDOW=OFF`, preserving
their device-free dependency set. 04c adds `shark.client.frame` with seed 1337,
labels `cpu;unit;client` and a 20-second timeout. Its deterministic input/time
fixtures and the expanded `shark.core.timing` cover pause/step, first-tick input,
failure/cancellation and server independence. Native window smoke separately
exercises the shared launch helper and reserved F6/F7 sample controls. Final local
qualification is PASS in the [04c handoff](handoffs/04c-frame-loop.md).
05a adds production device ownership and `shark.client.graphics` CPU fixtures,
`shark.client.graphics-cli` device-free command checks in native builds, and
`shark.tooling.graphics-device-evidence` parser tests. Native Windows clients
compile Vulkan/NVRHI by default; WSL CPU and Server builds keep
`SHARK_NATIVE_GRAPHICS=OFF`. Actual device qualification runs the explicit
[graphics device fixture](graphics-device.md) on both current-PC GPUs with core,
synchronization and NVRHI validation. It is separate from ordinary CTest and
does not establish presentation or GPU submission. No new hosted result is
implied by local work.
05b adds `shark.client.presentation` with seed 1337, labels `cpu;unit;client` and
a 20-second timeout for injected lifecycle, launch and owned queue-waiter cases.
Device-free CLI checks cover `PresentationSmoke` and Editor/Runtime `--graphics`;
`shark.tooling.presentation-evidence` checks the strict native-evidence parser.
Actual presentation qualification separately runs the
[presentation lifecycle fixture](presentation-lifecycle.md) on both local GPUs
in Windows Debug, Release and ASan with required validation. Ordinary CTest does
not open a window or replace those hardware runs. Editor/Runtime's clear sample
uses the shared offline frame/input pump; its diagnostic completion is NOT_RUN
for qualification. Full `REQ-05` remains NOT_RUN until 05c's sustained/fault gate.
05c's [qualification guide](presentation-qualification.md) declares the fixed
native workloads, resource limits and commands. `shark.client.presentation-faults`
executes three isolated CPU fatal-destructor paths with a 45-second CTest limit;
`shark.client.presentation-stress-cli` checks device-free discovery and invalid
arguments with a 30-second limit. The `shark.tooling.presentation-faults`,
`shark.tooling.presentation-stress` and `shark.tooling.runtime-package` tests reject
malformed or false evidence and invalid package inputs. Native recording/surface/
device fault injections, sustained measured presentation and relocated Runtime
execution are separate opt-in runs. Their wrappers bind the executable, source,
lock, tools, validation payload and selected UUID to retained raw records; a test
child's expected fatal exit is accepted only with its matching cleanup evidence.
Windows tests cover finite-duration shutdown; POSIX SIGINT/SIGTERM and readiness
I/O failure checks execute on Linux, not through Windows `terminate()`.

## Locked tools and hosted-image limits

[requirements-dev.txt](../requirements-dev.txt) locks these distributions with
SHA-256 hashes for Windows x64 and Linux x64:
CMake 3.31.10, Ninja 1.13.2, clang-format 20.1.8 and PyYAML 6.0.3. Python is exactly
3.13.5 for the retained desktop CPU workflow. The separately locked minimal
headless image uses Ubuntu snapshot Python 3.12.3, as described below; this does
not relax the existing CPU helper's pin. Install only wheels with the documented hash-checking command in
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
MSVC toolset family/SDK and GCC/Clang executable family; it fails when a requested
toolchain is absent instead of silently choosing another Visual Studio major
version. Full compiler patch/build versions, compiler binary
hashes, SDK selection and `ImageOS`/`ImageVersion` are recorded after configure.
Upstream inventory documents the selected labels and installed tools; actual
results and recorded images are linked in the handoff.
[GitHub runner image policy and inventory](https://github.com/actions/runner-images)

Qualification amendment to ADR 0002: exact project tools and source dependencies
are immutable inputs; hosted CI compiler families are explicitly selected
and their exact observed binaries recorded per run. This is a controlled rolling
CI configuration, not a claim that a hosted VM can be recreated from its label.
Pinning the action or Python version does not freeze the OS, standard library,
OS SDK, compiler patch or Python distributor rebuild. Assignment 35 must archive
the actual toolchain/environment and build inputs needed for release reproduction,
or use an immutable controlled runner image. A missing toolchain or changed compiler
requires a reviewed configuration/evidence update; do not mask that failure.

## Minimal Ubuntu headless gate

[inputs.json](../tools/ci/headless/inputs.json) pins the Ubuntu 24.04 amd64 base
image to `sha256:a2d43bffc46670d7108bb3c0468c67c5557c4c92e83e1e34cbff1322ad1b94dc`,
the APT snapshot to `20261017T000000Z`, and the bootstrap CA package by SHA-256.
The [Dockerfile](../tools/ci/headless/Dockerfile) and
[setup script](../tools/ci/headless/setup.sh) install GCC 13, Clang 18 and its
sanitizer runtime, Python/venv, Git and binutils from signed snapshot metadata
with recommendations disabled. The complete installed package/version/architecture
inventory must exactly match [packages.tsv](../tools/ci/headless/packages.tsv).
Presentation, source-import and offline shader packages are rejected separately.
The locked wheel tools are installed with hash checking. Python 3.12.3 is a
specific exception for this isolated image, whose Ubuntu package revisions are
recorded; ordinary CPU jobs continue to require Python 3.13.5.

On a native Linux x86-64 host with Docker and GNU `timeout` available, run the
same entry point used by CI, choosing a fresh evidence directory for each run:

```sh
bash tools/ci/headless/run.sh out/ci/headless-local
```

The [wrapper](../tools/ci/headless/run.sh) builds its image from a temporary
context containing only the image recipe and locked tooling inputs. It binds
the repository read-only at `/source`, including its existing Git metadata, and
writes builds inside the disposable container and evidence into the selected
host directory. It performs no staging, commits or source copies. Docker image
setup changes the disposable environment, not host APT configuration. Do not
run `setup.sh` against a working host installation. No Podman or alternate-host
qualification is implied by this Linux command.

When Docker is unavailable, the checked-in Linux/WSL fallback uses the same
hash-verified base userspace and snapshot setup inside private chroots:

```sh
python tools/ci/headless/fetch_inputs.py --output .tools/headless
sudo bash tools/ci/headless/chroot.sh /var/tmp/shark-headless-local .tools/headless out/ci/headless-chroot-local
```

Choose an unused `/var/tmp/shark-headless-*` parent and empty evidence directory.
The [fetcher](../tools/ci/headless/fetch_inputs.py) verifies the manifest, image
configuration, base layer and CA package against the input lock. The
[chroot wrapper](../tools/ci/headless/chroot.sh) requires Linux privileges for a
private mount namespace, mounts source read-only and installs packages only
inside the disposable build root. It keeps a second pristine base root for the
installed-Server smoke, with a separate network namespace. It retains both
private root filesystems for inspection rather than deleting them automatically.
This records the shared host kernel explicitly; it does not claim an independent
VM or a successful Docker run. Unlike the Docker smoke, this fallback does not
claim a read-only runtime root.

[headless_ci.py](../tools/headless_ci.py) validates the OS/architecture, exact
installed package inventory, image provenance and Python/tool pins. It rejects
mapped GPU/audio devices and display sockets and clears display/audio-session
environment variables. It runs these stages in fresh build directories:

1. `server-only`: GCC Release with Client, Editor and Tests disabled, dependency
   fetching disabled, explicit Server/Core boundary check, component install,
   source/build/package closure inspection and a finite installed-Server run.
2. `server-debug`: GCC Debug with the Core/Server Catch2 and process tests;
   CTest requires a nonempty selection and writes JUnit.
3. `server-asan-ubsan`: Clang 18 with ASan/UBSan and leak detection, running the
   same headless tests with failure halting enabled.

The wrapper then starts the installed Release Server in the unmodified pinned
base image with networking disabled and a read-only root filesystem. This last
smoke test uses no build tools or Python in the runtime environment. Headless
qualification requires that smoke result as well as the build/test summary;
unsetting display variables on a desktop runner alone is insufficient.

Closure checks inspect CMake's generated graph and file-api data, the compiler's
actual source/header dependencies and linker inputs, installed package contents,
and ELF direct/transitive library dependencies. Negative fixtures exercise
presentation/authoring leaks. The test framework may be fetched for test-enabled
configurations; it must not enter `server-only` or the installed package. These
checks qualify an idle process lifecycle and dependency boundary, not world
simulation, networking, persistence, player capacity or release deployment.

The selected output contains `summary.json`, toolchain reports, phase logs,
JUnit, installed package inventory, input/package provenance, license records,
the Server package and closure report. Docker adds its image inspection; the
chroot path records its shared host kernel and pristine runtime package inventory.
`clean-runtime.json`
records the separate base-image smoke. CI uploads it as
`headless-server-<run_attempt>` even on failure, with a 30-day retention period.
Preserve required acceptance findings in the M01 handoff before artifacts expire.
Each phase has a deadline and a nonzero result stops dependent work. An absent
test or clean-runtime result is unrun, never a passing gate.

## Shared local and CI command

After installing the locked tools and entering the appropriate compiler environment:

```text
python tools/ci.py --preset windows-debug
python tools/ci.py --preset windows-release
python tools/ci.py --preset windows-asan
python tools/ci.py --preset linux-debug
python tools/ci.py --preset linux-release
python tools/ci.py --preset linux-asan-ubsan
```

Run only the presets for the current OS. Windows requires the native x64 MSVC
developer environment; a plain shell with MSYS CMake earlier on PATH is not a
valid substitute. The helper verifies the Python and tool pins before configuring.
An explicit `CXX` environment value overrides the preset's portable compiler name;
CI uses this to select the versioned GCC/Clang binary.

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
both at `3620fab`. M01's separate proof uses
[injected run 39082507455](https://github.com/markdawes08/shark-engine/actions/runs/39082507455)
and [restored run 39083098689](https://github.com/markdawes08/shark-engine/actions/runs/39083098689),
both at `d658d69`; it includes the new headless job. The
[M01 hosted follow-up](handoffs/M01-ci-verification.md) records their actual
assertions, phase exits and remaining qualification limits.
Use this procedure when a future build/CI change requires renewed
failure-propagation evidence. Finish the first dispatch before starting the
second; the workflow's concurrency policy can cancel overlapping dispatches.

1. Open the repository's **Actions > CPU foundation > Run workflow** on the
   intended revision. Set `inject_test_failure` to **true**. The six CPU jobs
   must reach CTest, execute the deliberately failing test and fail. A tool-install,
   configure or compilation failure does not prove test failure propagation.
2. Save the run URL, commit, runner metadata, each CPU job's nonzero test-phase
   exit code and JUnit containing the injected test failure. `quality` and
   `runtime-only` may remain green; the overall workflow must be red. The M01
   `headless-server` job also receives the injection input; its test-enabled
   configuration must reach the real sentinel assertion, not fail preparation.
3. Dispatch the same revision again with `inject_test_failure` **false** (the
   default). All nine current jobs must pass, and no injected failure may remain in
   CTest. Save this run URL and artifacts alongside the failed-run evidence.
4. Update the current assignment's handoff and requirement status only after checking
   both actual runs and the two native CPU rows. Ordinary failed push runs do
   not prove deliberate test failure propagation. Until the injected failure
   and restored pass are verified on the same revision, a new red/green claim
   remains unverified. Keep the previously verified revision explicit.

The same real failure can be checked locally, for example in a Windows developer
shell: `python tools/ci.py --preset windows-debug --inject-test-failure` must fail;
then `python tools/ci.py --preset windows-debug` must pass. The second invocation
explicitly resets the cached option to OFF. Preserve the first output directory
before rerunning if both evidence sets are needed. Local proof meets the amended local failure-propagation gate; it does not claim
that a hosted job executed. Repeat a hosted proof only when separately verifying
hosted CI, without making access to a runner a local roadmap prerequisite.

For the headless profile, use a fresh output directory for each invocation:

```sh
bash tools/ci/headless/run.sh out/ci/headless-injected --inject-test-failure
bash tools/ci/headless/run.sh out/ci/headless-restored
```

The first command must return nonzero because CTest ran the injected assertion.
The second must finish all normal stages, including the clean-runtime smoke.
The package-only configuration keeps injection disabled because it has no Tests.
The headless driver stops at the first failed test phase; phases after that point
remain unrun in the injected evidence. The historical 01 red/green reports keep
their original fourteen-job/120-test counts and tested revision.

## Configuration verification

`python tools/check_ci_config.py` parses the YAML and checks matrix coverage,
immutable action references, timeouts, least-privilege checkout wiring and the
manual failure input. It does not execute GitHub expression evaluation or jobs.
`python tools/check_format.py` checks engine-owned C/C++ files against the pinned
formatter; `--fix` deliberately applies formatting. No future GPU, import or
release tests are invented for this foundation.
