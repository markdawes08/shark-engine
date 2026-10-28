# 14c: isolated standalone export acceptance

Date: 2026-10-28. Base: `f631476`, initially clean `main`.
**14c and full REQ-14 are PASS under current-machine scope and the approved
[backlog amendment](../backlog.md).** Next is assignment 15, terminal authoring
and the local structured protocol. No staging, commits or pushes were performed.

## Delivered behavior

The existing Runtime package checker now accepts `runtime-scene-export`. It
inspects the trusted manifest, exact payload/notices, startup identities and
shader provenance; one isolated CPU inspection binds the actual Runtime loader
to that content. It then captures two 160x120 frames on each explicit GPU and
checks report identity, finite bounded depth, exact capture inventory, balanced
GPU teardown and observed Runtime/loader/four-CRT/Windows-driver module closure.
Missing or altered payloads and authoring/compiler dependencies reject.

The existing PowerShell helper adds scene, inspection and no-argument modes.
Every scene check starts in a separate initially empty directory outside the
package and checkout. Child environment is cleared, PATH is system-only and
development overrides are absent. Driver-created output is recorded with bounded
path/type/size metadata. Successful working directories are removed; unexpected
failures retain their locations. Default startup requests normal closure after
one second of owned visible-window observation, with actual positive frame and
teardown evidence checked separately.

The [standalone export contract](../standalone-export.md) documents the expanded
`check` command and report. Runtime, Core, Client, Tools, package formats and
dependency inputs are unchanged. Python/PowerShell remain verification tools;
the exported executable has no dependency on either at startup. This is the
temporary static unlit client profile, not an editor, final cooked content,
dedicated server or an MMO release.

## Selected verification

Evidence is under `out/14c-work/`. The Windows Debug inventory was listed before
selection (`windows-inventory.log`). Package tooling is the changed owner; no
engine rebuild or unrelated matrix was needed. The existing Python CTest fixture
was invoked directly: `python tools/tests/test_runtime_package.py -v` passed all
32 cases in 5.150 s. Two affected cases were rerun after each focused review/fix;
the final pair passed in 0.554 s. Those cases cover scene completion/capture,
strict isolated reports, fresh per-launch working directories, driver-created
output and failure retention. PowerShell parsing and diff review also pass.

The actual package is the unchanged final 14b imported export:
`out/14b-work/imported-export`, manifest SHA-256
`3a70a15519e60b87948fc1050c15c8ebff28b7c6f64c66b77ea4ae52efc36eef`.
Its complete inventory was inspected before and after relocation. Native runs use
the standard-user wrapper with recorded medium-integrity token evidence. The
relocation and generated copies are identified by `deployment.json`; their path
contains spaces and Unicode and is outside the checkout.

| Check | Result and evidence |
| --- | --- |
| Isolated CPU identity inspection and both-GPU package check | PASS; `isolated-final/summary.json`, `isolated-final-token.json` |
| RTX 4070 Laptop, UUID `863d6f59a51894ab8b4829aa9460c78d`, driver 616.56 | Two presented frames, exact observed module closure; independent imported-scene oracle checks 18,640 pixels with zero color/depth error |
| Intel RaptorLake-S Mobile, UUID `868088a7040000000002000000000000`, driver 101.6790 | Same two-frame, module and 18,640-pixel result; `isolated-image-comparisons.json` records both |
| No-argument startup from different CWD | PASS, 35 frames, expected imported draw, observed module closure and balanced teardown; `default-startup.json`, `default-final-token.json` |
| Corrupted startup/scene/mesh/shader and missing mesh | Five expected nonzero structured failures from the isolated packaged CPU loader; `failure-controls.json`, `failures-token.json`; original bytes restored and inspected |
| Actual F5 rejection on RTX with required validation | Invalid descriptor diagnosed, instance 1 and imported image retained; two frames and 18,640 independently checked pixels |
| Actual F5 complete replacement on RTX with required validation | After expected invalid-candidate rejection, fresh built-in candidate installed as instance 2 with two draws; four frames and 18,206 independently checked pixels |

Both F5 phases have zero image/depth error, no Vulkan/NVRHI warnings/errors and
fully retired GPU owners. `reload-final/summary.json`, captures and
`reload-final-token.json` retain the observations. The fixture uses targeted
synthetic native focus/F5/F6 messages to the process-owned HWND, exercising real
GLFW translation and Runtime dispatch. Existing F6 acknowledgements synchronize
the frame loop; a second acknowledgement establishes that preceding reload and
presentation finished before normal close. This is not physical-keyboard or
Windows foreground-focus-policy qualification. The fresh candidate came from
the existing CPU worker (`fresh-content-worker-final.log`).

Primary isolated command (under `tools/windows_unelevated.py`):

```text
python tools/package_runtime.py check --package RELOCATED_PACKAGE --manifest-sha256 3a70a15519e60b87948fc1050c15c8ebff28b7c6f64c66b77ea4ae52efc36eef --device 863d6f59a51894ab8b4829aa9460c78d --device 868088a7040000000002000000000000 --output out/14c-work/isolated-final
```

The local acceptance driver `out/14c-work/acceptance.py` records default startup,
negative controls and native reload using existing helpers/oracles. It is retained
evidence, not a new versioned test framework. No image baselines, tolerances,
workloads, seeds or engine configuration changed.

## Investigated setup failures and scope

The first temporary relocation inherited an elevated-owner-only ACL, so its
standard-user launch could not enter the working directory. The current user was
granted access only to that generated test root, and the same copy then passed.
`default-token.json` retains the failed launch. Automatic approval review rejected
the attempted temporary-root deletion with only `blocked by policy`; no files
were removed by that command. The successful acceptance copies total 18.5 MiB;
their final cleanup remains a user action at the path in `deployment.json`.

The first RTX package check rendered correctly, but the checker incorrectly
required CWD to remain empty after launch. The NVIDIA driver creates empty
`NVIDIA Corporation/umdlogs` directories. The checker now requires emptiness
before launch, records output afterward and uses a fresh directory per child.
The failed result remains in `isolated/`; the corrected connected check passes.

The first F5 supervisor attempted to take foreground focus and received none;
Runtime completed its 15-second deadline normally without a reload. Its clean
1,276-frame result in `reload/` is not reload evidence. The final fixture uses
explicit synthetic notifications without taking the user's foreground app.
An initial worker preparation also rejected an incorrect shader path; the command
was corrected to the existing verified generation. Both worker logs remain.

Isolation runs deliberately disable validation to prove no validation SDK/layer
is needed at deployment. Independent images check their actual output; unchanged
13e both-GPU rendering/lifetime evidence and 14b's required-validation package
capture remain applicable. The new F5 path was exercised with required validation.
The checker itself keeps `qualification: NOT_RUN`, distinct from the assignment's
combined acceptance. Installed development tools are not physically removed, and
module polling cannot establish transient unload history.

Earlier 14a/14b Debug/WSL, export-worker ownership/publication, protected Release
Runtime and standalone CPU Tools results apply because their relevant inputs are
unchanged. Windows-only supervisor changes do not require WSL, Server, native
Linux graphics, new compiler builds or hosted CI. Existing 17 FeatureWorld cases
and public API mappings remain intact; the export mapping now links connected
deployment evidence. `.gitignore` already covers all generated output, caches,
local environments and the optional roadmap; no new ignore entry is necessary.

Final repository/link/manifest checks and `git diff --check` pass. Next work is
to plan assignment 15 around the shared authored-project owner, typed services,
noninteractive UTF-8 framing, discovery and bounded operation results. No editor
UI or second authoring mutation path is implied by this package milestone.

Suggested commit: `feat(export): qualify isolated standalone scene packages`.
