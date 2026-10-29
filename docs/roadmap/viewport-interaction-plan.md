# Assignment 17: viewport interaction checkpoints

Date: 2026-10-29. Base: `7a54cf2`, initially clean `main`. Owner: REQ-17.
This four-checkpoint plan follows [16e's headless preparation](../handoffs/16e-editor-terminal-acceptance.md).
The user's [desktop pause](../backlog.md#desktop-ui-deferral) permits continued
coding, without closing full REQ-16 or qualifying actual UI interaction.

| Checkpoint | Deliverable and acceptance |
| --- | --- |
| 17a: Viewport navigation and picking | Display-only orbit/pan/dolly, actual-image coordinate mapping including DPI/offsets, same-snapshot triangle picking and revision-safe selection. Validate the chosen view before culling; preserve authored cameras, source, dirty state and history. |
| 17b: Transform manipulation and drag transactions | Shared local/world transform math, parented/rotated parents, snapping, supported scale and explicit shear/singular rejection. Complete drag/cancel/conflict semantics, with one undo group per committed drag and equivalent terminal results. |
| 17c: Gizmo integration | Translate/rotate/scale controls through 17b's common validation/history path. Integrate focus, capture, cancellation and local/world modes; select/pin any new dependency before use. |
| 17d: Connected acceptance | Equivalent human/terminal transforms, stale requests, save/reopen and undo, including actual navigation/picking/gizmo input and the required native rendering checks. Reuse valid earlier focused results. |

17a is implemented with selected headless verification. Its
[navigation contract](../viewport-navigation.md) and
[handoff](../handoffs/17a-viewport-navigation.md) separate implemented behavior
from deferred actual input/GPU acceptance. 17b adds the shared
[transform and drag contract](../transform-manipulation.md); results and limits
belong to its [handoff](../handoffs/17b-transform-manipulation.md). 17c connects
[native gizmos and rendered previews](../viewport-gizmos.md), with selected
headless results in its [handoff](../handoffs/17c-viewport-gizmos.md).
17d connects CPU adapter and public terminal processes through saved projects;
its [handoff](../handoffs/17d-connected-viewport-acceptance.md) records the headless
results. The [actual acceptance procedure](../viewport-acceptance.md) and required
native checks remain deferred. Full REQ-17 stays NOT_RUN. Later coding may proceed
to assignment 18 under the user's explicit nonblocking desktop amendment.

Editor camera navigation and selection are presentation state, not authored
transform mutations or gameplay intents. Gizmo writes use the original
ProjectSession and shared authoring services. Keep scene-local coordinates and
bounded render origins distinct from global MMO cell identity; world streaming,
authority and physics remain with their existing owners.

Use the [testing policy](../testing-policy.md): focused CPU math/adapter checks,
affected renderer preparation and retained-owner regressions. Native windows,
input and GPU qualification stay NOT_RUN until explicit desktop resumption.
Record the required checks in the backlog; do not substitute synthetic input for
actual UI acceptance or repeat unrelated engine matrices.
