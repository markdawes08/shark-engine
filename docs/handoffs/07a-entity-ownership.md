# 07a: Entity ownership and identity

Date: 2026-10-18. Starting revision: `9e08702` on `main`, initially clean.
The user owns staging, commits and pushes. All changes remain unstaged.
**07a is complete under the current-machine scope. Full REQ-07 remains NOT_RUN.**

## Scope and implementation

This checkpoint implements private EnTT scene ownership, persistent identity
domains, checked process-local handles, immutable region/replica scope and
authority references, bounded deferred create/destroy, and canonical global-cell
coordinates. See the [contract](../scene-contracts.md) and
[07 checkpoint plan](../roadmap/scene-plan.md).

Scene access is confined to its creating thread. Reused slots cannot alias stale
handles; exhausted slots retire. Iteration passes owned values and accepts only
deferred writes. Callback failures cancel newly queued work; FIFO application
returns a separate outcome per operation. Duplicate IDs and stale references fail
without reviving old entities. World coordinates remain integer cells plus bounded
local doubles; transforms and render/physics origins are not implemented here.

The exact MIT-licensed EnTT archive is private to EngineCore and adds no window,
graphics, authoring or test dependency to Server. Disconnected Server builds use
an explicitly prepared locked archive. Closure verification checks populated
source bytes and the installed MIT license. Runtime notices include it too.

## Verification

The [durable receipt](../coverage/evidence/07a-entity-ownership.json) records source
identities, commands, test results, dependency closure, package files and limits.
All **348 CTests pass with no skips or failures**:

| Current-machine profile | Result |
| --- | --- |
| Windows MSVC Debug, Release, ASan | 58/58 each |
| WSL GCC CPU client Debug, Clang client ASan/UBSan | 51/51 each |
| WSL GCC Server Debug, Clang Server ASan/UBSan | 36/36 each |

The scene suite has 16 cases/25,611 assertions and coordinates have 8 cases/20,343
assertions in every profile, with seed 1337. Tests cover scoped references,
destroy/recreate/reload/reconnect, malformed handles, duplicate IDs, capacity,
FIFO failures, callback cancellation, owner-thread rejection and closed lifetime.
Coordinate cases cover integer boundaries, negative offsets, positive zero,
nonfinite values, round-to-nearest enforcement and 512 deterministic round trips.
Generation exhaustion is exercised through the production helpers; no test forces
an actual Scene slot through uint64 exhaustion or the private EnTT version wrap.

Windows `runtime-only` and `server-only` configure/build and their protected
boundaries pass. Actual Server lifecycle and installation pass. WSL Server Release
passes target/source/ELF/package closure, all 374 EnTT source files match the pinned
archive, and the installed executable plus MIT notice run in the retained SDK-free
Ubuntu userspace with an isolated network. WSL sanitizer checks use leak detection
and halt-on-error. Presentation dependencies remain absent from headless builds.

A fresh Windows Runtime package contains the EnTT MIT notice and passes file/DLL
closure. Its copied executable runs `--version` and `--help` outside the checkout
in a Unicode/space path with system-only PATH and cleared developer overrides.
Deleting the notice from a separate test copy makes package inspection fail.
This is CPU deployment evidence; the earlier cube/GPU qualification is unchanged.
Windows still has development tools installed. WSL shares its kernel and uses a
writable retained runtime rootfs; no separate physical-machine claim is made.

Formatting passes for 111 C/C++ files. Contract checks pass 29 negative cases;
repository/ignore checks pass three negative skill cases and document links.
Public-header and actual compiler-include inspection confirms that only Core
implementation files receive EnTT includes in all three Windows profiles.

The initial unquoted PowerShell CMake path was split at the drive colon; using an
explicit `-CommandArguments` string array fixed the invocation. An extra runtime
audit initially mistook Ubuntu's `gcc-14-base` documentation/metadata package for
a compiler; direct file inventory and executable absence checks corrected that
audit. Neither required engine or host changes. Both are retained in local evidence.

Reproduction entry points retained under ignored `out/07a-work/`:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07a-work/verify-windows.py')
wsl -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/07a-work/wsl-run.sh
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07a-work/verify-runtime-package.py')
```

These evidence drivers require fresh output directories. Durable source commands
are in [building](../building.md), [CI](../ci.md) and the receipt. Toolchains:
MSVC 19.51.36248, VC 14.51.36231, SDK 10.0.28000; WSL GCC 13.3/Clang 18.1.3;
CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8. No host settings changed.

## Continue

07b is next: Name, local Transform, centrally configured math, hierarchy and
validated reparent/delete behavior. Full `REQ-07` remains NOT_RUN until 07b/07c
complete. Scene files, authoring transactions/commands, editor UI, networking,
distributed ownership and MMO capacity are not claimed by this checkpoint.
FeatureWorld connects the implemented components in assignment 10. The ignored
local roadmap and generated artifacts stay outside version control.

Suggested commit: `feat(core): add scoped entity ownership and deferred scene mutations`
