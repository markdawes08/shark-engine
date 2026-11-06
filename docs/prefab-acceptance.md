# Connected prefab acceptance

21e connects the existing [source](prefab-assets.md), [authored instances](prefab-instances.md),
[runtime mutation](runtime-spawning.md) and [Editor/terminal workflow](prefab-authoring.md).
Use the [testing policy](testing-policy.md) and reuse applicable 21a–21d results.
The native Editor interaction deferred in BL-21D-UI-01 remains a separate gate.

## Source to runtime values

`prepare_runtime_prefab` in
[prepared_content.hpp](../engine_tools/include/shark/tools/prepared_content.hpp)
validates a PrefabDocument, copies its current component values and prefab-local
references, then validates the complete Core RuntimePrefab, including composed
transforms. The returned value owns its data. Source ProjectId and extras stay
in Tools; mesh IDs and inert script references retain their meanings.

This pure conversion reads no file, imports no mesh, cooks no package, creates no
entity and grants no authority. The trusted project/host owner must check source
identity, content closure and runtime authority at their existing boundaries.
Runtime and Server retain their source parser exclusion. There is no new source,
cooked, protocol, database or Lua version.

## Automated gates

| Gate | Independent observations |
| --- | --- |
| Saved authored instances | Read a real prefab/sidecar pair. Create 32 three-entity instances with independent IDs and literal local/world positions. Edit one child, save/reload and reopen, preserve the others and source extras, reject old handles and foreign retry scopes, and retain the original same-owner retry receipt. Later source edits cannot retarget prior instances or owned runtime values. |
| Callback mutation and runtime lifetime | Use actual native contact observations to spawn during Lua callbacks. Check whole-batch deferral, exact replay, complete transforms/bodies before init, instance-local changes, reverse script/body/entity cleanup, stale observations and capacity return across repeated cycles. |
| Replacement and reload | Reload captured source into fresh Scene/runtime/physics owners and authority epochs. Preserve owned historical receipts while old entities, bodies and grants reject against the replacement. Tear down in reverse order. |
| GPU lifetime | Run `SceneRenderSmoke --spawning` through `tools/scene_render_smoke.py --mode spawning`, serially on the RTX and Intel GPUs with required validation. Two runtime incarnations each perform four spawn/render/destroy/render-empty cycles. Compare all 16 actual color/depth readbacks against literal occupied/empty references; check completed GPU retirement and final viewport resource closure. |

Use fixed seed 1337 and explicit controlled updates/native steps. The CPU
acceptance CTest has a 60-second timeout. Native child processes retain the
existing scene-render supervisor timeout and image tolerances. CPU extraction
proves snapshot ownership/removal; actual readbacks and resource accounting prove
the GPU gate. Owned resource counters do not measure driver allocations or prove
production server capacity.

Manual native Editor checks remain NOT_RUN until the user resumes them. Automated
offscreen rendering does not qualify actual Prefabs tab input. Runtime actors
remain ephemeral; authenticated movement, network reconnect/handoff and durable
reward effects retain M04/M05 and their existing transaction owners.
