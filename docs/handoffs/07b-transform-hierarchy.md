# 07b: Name, Transform and hierarchy

Date: 2026-10-18. Starting revision: `681bbf4` on `main`, initially clean.
The user owns staging, commits and pushes. Changes remain unstaged.

## Implementation

07b adds mandatory Name/Transform values to the existing Core scene, validated
parent/child links, iterative subtree deletion, depth bounds, cached world matrices
and explicit keep-local/keep-world reparenting. Immediate and deferred operations
share the same single writer and checked lifetime boundary. Every affected subtree
is validated before publishing a change. Name reuses Core's UTF-8 validation.

The [transform contract](../transforms.md) fixes the numerical rules and the
distinction between scene-local matrices and global world cells. Centrally
configured GLM is pinned, with its MIT notice in both Runtime and Server packages.
The public changes are [Name](../../engine/core/include/shark/name.hpp),
[Transform/math](../../engine/core/include/shark/transform.hpp) and
[Scene](../../engine/core/include/shark/scene.hpp); private implementation, CMake
dependency setup, boundary/package tooling and focused tests change with them.
No renderer, editor adapter, world-origin or network protocol is introduced.

## Verification

All **362 CTests pass with no failures or skips** under the current-machine scope:

| Profile | Result |
| --- | --- |
| Windows MSVC Debug, Release, ASan | 60/60 each |
| WSL GCC CPU client Debug, Clang client ASan/UBSan | 53/53 each |
| WSL GCC Server Debug, Clang Server ASan/UBSan | 38/38 each |

The hierarchy suite has 22 cases/3,892 assertions and the transform suite has
12 cases/5,385 assertions, with seed 1337 in all seven profiles. They cover
UTF-8 Name validation, owned snapshots, stale/foreign/authority references, cycles,
1024-level iterative deletion, FIFO revalidation and callback cancellation,
nonuniform/negative scales, both reparent policies and unchanged state after
failure. Independent analytic matrices and 512 deterministic transform round trips
check the math independently of registry storage. Existing scene, world-coordinate,
log/path and lifecycle regressions remain active. Sanitizer runs passed;
Linux uses leak detection and halt-on-error for ASan/UBSan.

The dependency suite passes seven cases, including actual external C++ consumer
compilation, rejection of prior GLM inclusion and 20 conflicting definitions.
Actual Windows compile databases confirm private EnTT and the same five GLM
definitions for all 67 engine-owned consumers in each profile. All 27 public
headers exclude EnTT; math.hpp is the sole direct public GLM include.

Protected Windows Runtime-only and Server-only builds pass, with actual Server
lifecycle and installation. WSL Release Server passes target/source/link/ELF and
package closure: all 374 EnTT files and 1,674 GLM files match the locked archives.
The installed Server and both MIT notices run in the retained SDK-free Ubuntu
userspace with an isolated network. Its 92 installed package inventory is unchanged.

A fresh Windows Runtime package passes file/DLL closure, contains both notices,
and runs help/version outside the checkout in a Unicode/space path with system-only
PATH and cleared developer overrides. Removing either notice from a separate
test copy correctly fails inspection. This is CPU deployment evidence. Windows
development tools remain installed; WSL shares the host kernel and uses a writable
retained runtime rootfs. No additional GPU or physical-machine qualification is claimed.

The final shell audit caught CRLF line endings in the updated headless CI wrapper.
After the CPU matrices completed, that file alone was normalized to LF and its
syntax/runtime audits repeated. The receipt preserves the failed audit, exact
before/after hashes and this shell-only amendment; C++ inputs and binaries were
unchanged. The formatter also required the documented pinned developer-shell PATH.
Formatting passes for 119 C/C++ files. Contract checks pass 29 negative cases
and validate all 50 requirements across 46 assignments; repository/ignore checks
pass three negative skill cases. The final diff passes whitespace checks and
the staging index is unchanged.

The [receipt](../coverage/evidence/07b-transform-hierarchy.json) records source
identities, commands, toolchains, raw artifact hashes, outcomes and limits.
Reproduction entry points retained under ignored `out/07b-work/`:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07b-work/verify-windows.py')
wsl -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/07b-work/wsl-run.sh
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07b-work/verify-runtime-package.py')
```

These evidence drivers require fresh output paths. Durable configure/build/test
commands and offline archive preparation are in [building](../building.md) and
[CI](../ci.md). Toolchains: MSVC 19.51.36248, VC 14.51.36231, SDK 10.0.28000;
WSL GCC 13.3/Clang 18.1.3; CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8.
No host settings changed. Changes remain uncommitted at base
`681bbf4397e6623b2ebfbf242b46040da22669aa`.

Coverage registers `component.name`, `component.transform` and
`core.transform-hierarchy` against implemented CPU fixtures. FeatureWorld connects
in assignment 10. Full REQ-07 remains NOT_RUN until 07c completes schema and
semantic inspection. The strict scale floor and fixed tolerances can conservatively
reject keep-world at numerical thresholds; failure preserves the original subtree.
Storage/depth bounds do not establish MMO capacity or general allocation-failure recovery.

## Continue

07c is next: engine-owned Name/Transform schemas and bounded immutable revisioned
semantic queries. Persistence, transactions, FeatureWorld, terminal/editor adapters
and Lua retain their later owners. The current-machine scope is unchanged.

Suggested commit: `feat(core): add validated transforms and scene hierarchy`
