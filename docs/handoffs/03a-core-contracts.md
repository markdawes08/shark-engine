# 03a - Core contracts

Date: 2026-10-18. Base: `7714f64` on `main`, initially clean.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.

**03a is complete within its checkpoint scope.** Full `REQ-03` remains NOT_RUN
until 03b lifecycle and 03c scheduling/queue qualification. The exact next task
is **03b, explicit application ownership and lifecycle**, under the
[03 plan](../roadmap/core-plan.md). No work from assignment 04 is started.

## Delivered behavior and compatibility

The [Core API guide](../core-contracts.md) describes the contracts and limits.
EngineCore now owns `Error`/`Result<T>` with stable validation, unavailable,
conflict and execution categories; retained strings have no borrowed input
lifetime. `Logger` writes bounded UTF-8 JSON diagnostics to an explicitly
borrowed sink, with tested escaping, validation and write/flush failure behavior.
It is synchronous, allocates and requires serialized ordinary-thread calls.
Signal/audio/real-time callbacks cannot use it; the existing Server signal
handler remains an allocation-free `sig_atomic_t` assignment.

Bootstrap and Server parsers now return owned results. Directly constructed
Server configuration is validated before installing handlers or reporting
readiness. Help/version/readiness/completion stdout stays unchanged. Invalid
input emits a structured stderr line plus readable usage and exits 2; execution
or diagnostic-output failure exits 1. Runtime/Editor also detect failed output.
The old invalid-action alternatives and borrowed Server error field are removed
from the internal C++ API, with all consumers migrated. No data format migration
or game-authoring protocol is introduced.

Stable IDs validate canonical non-nil UUIDv4 text, use OS entropy in production,
and support deterministic injected entropy/failures in tests. Explicit domain
wrappers prevent accidental interchange of later authored and MMO IDs; no
durable database, game-domain catalog or runtime handle implementation is claimed.

Project paths preserve UTF-8 Unicode/spaces, reject malformed and escaping
references, distinguish portable references from native paths, and resolve
against an existing canonical root. Tests cover real filesystem links, root
replacement, missing descendants and malformed native encoding/NUL. The
resolver is not a race-proof sandbox and never creates files.

New public headers are `engine/core/include/shark/{error,log,stable_id,project_path}.hpp`.
Their implementations, existing bootstrap/configuration code and all three
executable roots are integrated. Windows entropy uses a classified imported
SDK `bcrypt` target; Linux uses libc `getrandom`. No fetched dependency, network,
graphics, audio or authoring library enters Core/Server. Existing boundary
checks and isolated builds pass.

## Verification and evidence

The [durable receipt](../coverage/evidence/03a-local.json) records the base SHA,
changed source/test digests, exact commands, artifact digests, toolchain context,
results and limits. Raw output stays ignored in `out/`.

| Configuration/check | Result |
| --- | --- |
| Native Windows Debug | 22/22 CTests PASS |
| Native Windows Release | 22/22 CTests PASS, including real structured process diagnostics |
| Native Windows AddressSanitizer | 22/22 CTests PASS |
| WSL pinned Ubuntu 24.04 GCC Debug, headless | 20/20 CTests PASS |
| WSL pinned Ubuntu 24.04 Clang ASan+UBSan, headless | 20/20 CTests PASS |
| Final rebuilt Core family in all five configurations | 31 cases, 1008 assertions each, seed 1337; zero Core skips |
| Windows `runtime-only` and `server-only` | Configure/build, protected boundaries and executable smoke PASS |
| WSL Release `server-only` | Configure/build/install, compiler/header/link/package closure and installed smoke PASS |
| Separate SDK-free WSL userspace | Fresh installed Server path with isolated network: timed startup/shutdown PASS |

Full regressions passed before the final root-replacement test was added. The
five focused Core reruns include that final test. The final WSL Release refresh
also rechecks the actual package against current source/header hashes. Original
run evidence is retained, rather than overwritten by follow-ups.

Windows: Windows 11 build 26200 x64, MSVC tools 14.51.36231, SDK 10.0.28000.0,
Python 3.13.5. WSL: existing kernel 6.18.33.2, pinned Ubuntu 24.04 headless
userspace, GCC 13.3.0/Clang 18 and Python 3.12.3. Pinned development tools:
CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8. No host package, driver,
WSL resource setting, hosted workflow or remote branch policy changed.

Full native commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-release --jobs 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-asan --jobs 1
```

The ignored `out/03a-work/verify-final-windows.py`, run through the same wrapper,
serializes these final commands for each native preset and then both isolated
presets. Exact expanded arguments and exit codes are in the receipt:

```text
cmake --build --preset <windows-preset> --parallel 1
ctest --preset <windows-preset> -R ^shark.core.contracts$ --output-on-failure --no-tests=error --output-junit <output>
cmake --preset runtime-only
cmake --build --preset runtime-only --parallel 1
cmake --preset server-only
cmake --build --preset server-only --parallel 1
build/runtime-only/bin/Runtime.exe --version
build/server-only/bin/Server.exe --run-for-ms 20
```

WSL commands use private mount namespaces, the retained pinned build/runtime
roots, read-only source and fresh output/package directories:

```text
wsl --user root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/03a-work/run-headless-recheck.sh
wsl --user root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/03a-work/retest-headless.sh
wsl --user root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/03a-work/retest-release.sh
```

The first invokes `python /source/tools/headless_ci.py --build-root
/work/build-03a-recheck --output /evidence --jobs 2` inside the existing minimal
Ubuntu root. Follow-ups rebuild the final Core fixtures and refresh Release
closure/package evidence. Logs are under `out/ci/03a-headless-recheck`,
`out/ci/03a-headless-final` and `out/ci/03a-headless-release-final`.

Final repository checks all PASS using `.tools/venv` Python: pinned `python tools/check_format.py`,
`python tools/validate_contract.py`, its `--self-test`,
`python tools/check_repository.py --self-test`, and `git diff --check`.
Formatting covers 35 C++ files; contract self-tests include 29 negative cases;
repository checks confirm five discoverable skills and ignore boundaries.
An initial standalone repository check used system Python without PyYAML;
the required pinned interpreter passed without installing or changing packages.

Initial failures were fixed and retained: imported SDK target visibility needed
`GLOBAL`; C++20 deprecated `u8path` was replaced; Windows SDK includes needed
their dependency order preserved during formatting. One initial ASan compilation
reported MSVC C1090/PDB RPC error 0x6BA while another configuration built. A
serialized one-job retry passed without a code change; the underlying transient
cause is not established. No warning policy or test tolerance was weakened.

## Coverage, limits and next checkpoint

Two implemented-feature entries map these contracts to `shark.core.contracts`,
existing bootstrap/configuration tests and real process fixtures. Component,
authoring-command and Lua arrays remain empty. Temporary filesystem and process
fixtures are justified external coverage; FeatureWorld does not exist until 10.
The requirements manifest retains owner/current assignment `03` and its overall
NOT_RUN status, with 03b/03c explicitly outstanding. Current README, AGENTS,
architecture index, MMO/agent plans and ignored roadmap point to 03b.

All new Core link/Unicode/entropy cases executed on both Windows and WSL; no
Core case skipped. Junctions have no separately constructed fixture. Existing
platform-specific tooling tests retain their explicit skips: POSIX signal and
read-only descriptor process tests execute in WSL, not Windows. GPU/audio,
graphical editor, scene/authoring, persistence/network/MMO behavior and production
capacity are not qualified here. No new hosted run was needed or claimed.

`.gitignore` was reviewed: it already covers the local roadmap, build/output
directories, caches and generated fixture evidence. No new rule is necessary;
all new source/tests/docs remain eligible for version control. Nothing was
staged, committed or pushed.

Suggested commit: `feat(core): add errors, configuration, IDs and project paths`
