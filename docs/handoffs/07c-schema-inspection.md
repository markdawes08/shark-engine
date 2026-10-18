# 07c: Component schemas and semantic scene inspection

Date: 2026-10-18. Starting revision: `a475da7` on `main`, initially clean.
Changes remain uncommitted; the user owns staging, commits and pushes.

## Delivered contracts

EngineCore publishes owned, versioned Name/Transform metadata and pure validation
through [schema.hpp](../../engine/core/include/shark/schema.hpp). Defaults, units,
constraints and canonicalization match the existing components. Unknown schema
IDs, versions, property IDs and value types fail explicitly.

[Scene queries](../../engine/core/include/shark/scene_query.hpp) provide immutable,
owned pages over committed scene state, with independent row and scan limits,
identity/authority context, optional component groups, hierarchy selectors and
UTF-8 name prefixes. Opaque cursors preserve options and bind continuation to the
scene incarnation and revision. Direct entity inspection has the same lifetime
and revision rules without copying unbounded child lists. See the
[inspection contract](../scene-inspection.md) for exact limits and examples.

These are shared CPU C++ contracts. Scene files, authoring transactions, terminal
adapters, editor UI, Lua and gameplay networking retain their later owners.

## Verification

**07c and full REQ-07 are PASS** under the current-machine scope. The
[receipt](../coverage/evidence/07c-schema-inspection.json) records exact source
identities, commands, raw artifact hashes, outcomes and limits.

All **376 CTests pass with no failures or skips**:

| Profile | Result |
| --- | --- |
| Windows MSVC Debug, Release, ASan | 62/62 each |
| WSL GCC CPU client Debug, Clang client ASan/UBSan | 55/55 each |
| WSL GCC Server Debug, Clang Server ASan/UBSan | 40/40 each |

The schema suite has 9 cases/654 assertions; queries have 19 cases/3,916 assertions.
All use seed 1337. Schema tests compare defaults, units, limits and canonicalization
with actual component validation and Scene behavior, including invalid versions,
IDs, value types, nonfinite values, scale boundaries and Unicode controls.
Queries cover all field groups, sparse empty-page progress, UTF-8 prefixes,
54 selector/filter/budget combinations against independent expected sequences,
replay, every mutation kind, no-op and failed changes, deferred state, lifetime,
thread rejection and copied scope/authority. Earlier scene, global coordinate,
transform and hierarchy tests remain active in every profile. ASan/UBSan runs
passed, with leak detection and halt-on-error on Linux.

Review before qualification found that an implicit cursor move could empty its
source name filter while preserving its revision and position. The final API
uses copy-only cursor semantics; both original and destination preserve the
options on rvalue construction/assignment. A dedicated regression passes in all
profiles. No source changes or qualification exceptions were needed after freeze.
The final WSL audit verifies all 221 build/test/tool inputs unchanged; the combined
receipt checks both platform source maps against the final worktree.

Actual Windows compile databases confirm private EnTT and consistent public GLM
configuration across all 70 engine-owned consumers. All 29 public headers exclude
EnTT; math.hpp remains the only direct GLM include. The complete published C++
query example compiles independently with MSVC `/W4 /WX` using only public include
roots and central GLM definitions; its compiler-discovered inputs contain no
private engine or EnTT headers. No dependency pins changed in this checkpoint.

Windows Runtime-only and Server-only configure/build and protected boundaries
pass, with actual Server lifecycle and installation. WSL Release Server passes
target/source/link/ELF/package closure, including exact EnTT/GLM source identities
and both notices. The installed Server runs in the retained SDK-free Ubuntu
userspace with an isolated network; its 92-package inventory is unchanged.

A fresh Windows Runtime package passes file/DLL closure and runs help/version
outside the checkout in a Unicode/space path with system-only PATH and cleared
developer overrides. Removing either dependency notice from separate test copies
correctly fails inspection. Windows development tools remain installed; WSL shares
the host kernel and uses a retained writable runtime rootfs. These are local CPU
deployment checks, with no new GPU or independent physical-machine claim.

Formatting passes for 124 C/C++ files. Contract checks pass 29 negative cases;
repository/ignore checks pass three negative skill cases. Final document links,
whitespace and unchanged staging-index checks pass. Generated evidence stays
under ignored `out/07c-work/`; the local roadmap remains ignored.

Reproduction entry points retained on this machine:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07c-work/verify-windows.py')
wsl -d Ubuntu -u root -- bash /mnt/c/Users/mrkda/Projects/shark/out/07c-work/wsl-run.sh
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/07c-work/verify-runtime-package.py')
```

These drivers require fresh output paths; they reuse unchanged locked archives
under `out/07b-work/dependencies/`. Durable source commands are in
[building](../building.md) and [CI](../ci.md). Toolchains: MSVC 19.51.36248,
VC 14.51.36231, SDK 10.0.28000; WSL GCC 13.3/Clang 18.1.3; CMake 3.31.10,
Ninja 1.13.2 and clang-format 20.1.8. No host settings changed. Base revision is
`a475da7c9699cf2a0620d95748b3ac293a8a704e`; no result commit was created.

Coverage adds `core.schema-inspection` and extends `component.name` and
`component.transform` with implemented schema/query fixtures. Command and Lua
arrays remain empty. The fixed schema version is independent of future file,
authoring-protocol, gameplay-wire and database versions. These query bounds are
storage/work limits, not measured latency or MMORPG capacity; general allocation
exhaustion recovery is not guaranteed.

## Continue

M02 follows completed assignment 07: authenticated sessions and the replication
foundation, under the existing local MMORPG execution order. No M02 work starts
in this checkpoint. FeatureWorld connects the supported components in assignment 10.

Suggested commit: `feat(core): add component schemas and revisioned scene queries`
