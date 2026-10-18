# M01: Headless server and Core/Client separation

Date: 2026-10-17. Base revision: `e2f4966bd99797e7109397728f4ae2babce48f17`
on `main`, initially clean. Result: uncommitted working-tree changes; the user
owns staging, committing and pushing. M01 is current. Hosted verification for
this change set is pending; the historical 01 pass remains tied to `3620fab`.

## Delivered behavior and contracts

- EngineCore contains version/argument behavior and the strict ServerConfig
  parser. EngineClient contains client bootstrap help. Engine is an INTERFACE
  composition through EngineClient to EngineCore. EngineTools now uses Core;
  Editor uses Tools/Client, Runtime uses Engine, Server uses Core only.
- Server runs an actual headless idle lifetime, supports `--help`, `--version`
  and `--run-for-ms N` (1-6 decimal digits, value 1..600000), rejects invalid
  configuration before readiness with exit 2, reports startup/output failures
  with exit 1 and returns 0 after successful shutdown. SIGINT/SIGTERM request
  shutdown on POSIX; RAII restores handlers, including after partial startup.
- `server-only` builds Release with no Client/Editor/Tests or dependency fetch;
  `server-debug` and Linux `server-asan-ubsan` exercise real parser/lifecycle tests.
  A separate Server install component contains only the executable. It is a
  package-closure fixture, not a licensed shipping distribution.
- Target-domain checks protect Core/Server separately from clients and reject
  direct/transitive/generated presentation or authoring leaks. Independent Linux
  closure inspection audits file-api/cache/source, compile commands, package
  contents and the packaged ELF's resolved library closure.
- Minimal Ubuntu 24.04 CI adds a separate headless gate; existing client CPU
  jobs remain. Exact inputs and commands are in [CI](../ci.md).

No network listener, simulation scheduler, database, world state, renderer or
editor GUI is added. ADR 0007's 20 Hz / 256 active-player profile remains a
provisional target. The 20 ms idle polling interval is not a simulation tick.
General subsystem lifecycle/structured errors belong to 03. CLI bootstrap flags
are not authoring API commands; no data-format migration is introduced.

## Verification record

Tests ran on the uncommitted worktree. Hosted jobs cannot test these changes
until the user commits and pushes them. Local Windows 11 x64 evidence uses
MSVC 19.51.36248.0 (toolset 14.51.36231), Windows SDK 10.0.28000.0 and Python
3.13.5. The hosted Windows SDK selection remains 10.0.26100.0; these are distinct
environments. Commands below use the wrapper from [building](../building.md).

| Check | Result and evidence |
| --- | --- |
| `python tools/ci.py --preset windows-debug --jobs 2` | PASS, all 15 CTests; `out/ci/windows-debug/` |
| Same command, `windows-release` and `windows-asan` | PASS, all 15 CTests in each; `out/ci/<preset>/` |
| `cmake --preset runtime-only`, then matching build | PASS, client composition and both applicable boundary roots; Runtime `--version` prints `Shark Runtime 0.1.0` |
| `cmake --preset server-only`, then matching build | PASS, Core/Server only; `Server.exe --run-for-ms 100` starts and stops successfully |
| `python tools/check_clean_source.py --preset windows-debug --jobs 2` | PASS, fresh eligible-source configure/build/14 CTests, without local roadmap; `out/clean-source-20261017T234239712435Z/` |
| Same command, `server-debug` | PASS, fresh isolated-server configure/build/13 CTests; `out/clean-source-20261017T234617176925Z/` |
| Formatting, contract/negative checks, repository/ignore self-test, CI configuration | PASS; CI syntax does not execute GitHub jobs |

The source snapshots preceded final closure refinements for Ninja linker records,
prebuilt-object rejection and the explicit C++ module-scan setting. Final Windows
Debug/Release/ASan runs cover those changes; all 45 CTests passed. The final
focused boundary/tooling rerun also passed all five selected cases, including
25 closure regressions and four environment-audit tests.
Initial failures caught and repaired a fixture build-path false positive and
Ninja's additional linker dependency record. GNU's implicit `stdc-predef.h`
also exposed a fixture assumption; the fixture now verifies standard-header
package ownership, as the actual closure check already did. The minimal Clang
installation exposed CMake's automatic C++20 module scanner dependency. Shark
does not use module import units, so module scanning is explicitly off; ordinary
header dependency tracking and sanitizer instrumentation remain enabled. No
sanitizer finding was suppressed.

The new `headless-server` job and retained Windows/Linux/both-Mac CPU jobs are
NOT RUN on this change set. M01's gate remains open until that hosted evidence
is inspected. `headless-server` also needs adding to required checks after its
first verified run; the existing 14-check policy is unchanged.

Local Linux qualification uses the pinned Ubuntu 24.04 amd64 userspace in a
private chroot under WSL, with GCC 13.3.0, Clang 18.1.3, glibc 2.39 and Python
3.12.3. Its 187 installed packages exactly match the reviewed lock. The visible
host is an Intel Core i9-14900HX with 32 logical CPUs and 16,221,888 kB memory;
builds use two jobs. The helper adds no CPU/memory/IO quota. This is a shared
WSL kernel, not bare-metal Ubuntu or a load/capacity qualification.

GCC Debug and Clang ASan/UBSan each pass 13/13 CTests. Leak detection and
halt-on-error remain enabled. Release builds and installs with exactly Core,
Server and its boundary utility. The independent inventory verifies actual
compiled sources/headers, archive/link inputs, package contents and resolved
ELF libraries: `libstdc++`, `libgcc_s`, `libc`, `libm` and the Linux loader only.
The pristine pinned base also passes the real lifecycle test with no source or
compiler mount and isolated networking. Its rootfs is writable; the Docker
wrapper's read-only-root variant remains a hosted check. The runtime needs no
Python; its Python test controller runs outside that pristine root.

Retained Linux evidence includes `out/ci/headless-clean-runtime/` (full installed
package lifecycle), `out/ci/headless-asan-recheck/` (13 sanitizer CTests), and the
earlier failure/recheck logs under `out/ci/headless-local-{b,final,green}/`.
No Docker executable is installed locally; the Docker wrapper has syntax/config
validation, while the actual local route uses the documented chroot helper.

Final local failure-propagation proof is **PASS**. The injected helper returned
8 after executing all 13 Debug CTests: only `shark.failure-sentinel` failed and
the other 12 passed. The subsequent restored helper returned 0, completed all
14 configure/build/test/package phases and passed 26/26 Debug/sanitizer CTests.
It then passed the pristine-base package smoke. The installed binary's SHA-256
matches the separate full pristine-runtime lifecycle test:
`405b5b069fe3ab56e38c26c96ee9e4e6d49176410e512a9292439efaf06cf213`.

The final evidence is `out/ci/headless-local-injected/` and
`out/ci/headless-local-restored/`, including `summary.json`, JUnit, phase logs,
toolchains, licenses and closure inventory. The [durable local receipt](../coverage/evidence/M01-local.json)
preserves results, commands, tested implementation hashes and environment details.
Earlier failures remain retained separately. This local proof does not replace
the pending GitHub jobs or exercise the Docker wrapper.

The prepared root lives at `/var/tmp/shark-headless-m01-final/` inside the WSL
`Ubuntu` distribution; it uses kernel `6.18.33.2-microsoft-standard-WSL2`.
The versioned fresh-environment route is documented in [CI](../ci.md).
After preparing that environment, the proof invoked
`python /source/tools/headless_ci.py --build-root /work/qualified-injected --output /evidence --inject-test-failure`,
then the same command with `/work/qualified-restored` and no injection option.
Each had a fresh evidence bind mount; exact local namespace/chroot drivers are
retained as `local-driver.sh` in their evidence directories. No host APT packages
or global Git configuration were changed.

The Windows tests explicitly report POSIX signal/read-only-descriptor cases
NOT RUN; finite-run shutdown is covered there. Linux exercises readiness,
SIGINT/SIGTERM, maximum-duration interruption and failed-readiness cleanup.
Signal completion must finish within two seconds; process readiness/smoke checks
have five-second deadlines. These are test bounds under the recorded environment,
not hard real-time promises under blocked output or host starvation.

## Coverage, repository state and next work

Server lifecycle and Server/Core closure have implemented-surface mappings in
[requirements.json](../coverage/requirements.json). FeatureWorld starts in 10;
these process/build/package fixtures are intentionally external. Networking,
persistence, streaming and capacity requirements remain NOT_RUN.

`.gitignore` covers all generated `build/`, `out/`, `.tools/` and package staging
paths, including the minimal Ubuntu inputs; source/tests/docs remain eligible.
The ignored local roadmap remains optional and is excluded from source snapshots.
Snapshot generation handles unstaged file moves without touching the index.

Next: close M01's hosted headless and retained client-matrix gate, then assignment
02, the real-hardware GLFW/NVRHI/Vulkan feasibility probe. A headless pass does
not waive any of the nine graphics hardware rows or either Mac CPU family.

Suggested commit: `feat(server): add headless foundation and core/client boundaries`
