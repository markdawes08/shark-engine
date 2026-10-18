# Name, Transform and hierarchy

07b extends the existing [scene owner](scene-contracts.md) in EngineCore. Every
entity has mandatory Name and Transform values. Callers use the same checked
handles, single writer and deferred queue as 07a. This is the shared CPU foundation
for later editor and terminal services. 07c implements the
[fixed schemas and bounded revisioned inspection](scene-inspection.md); file
persistence, transactions and adapters retain their owners in 08, 09, 15 and 16.

## Name

[Name](../engine/core/include/shark/name.hpp) defaults to `Entity`. It contains
1..256 UTF-8 bytes, rejects malformed encodings and C0/C1/DEL controls, and preserves
valid bytes without case folding or Unicode normalization. Spaces and non-ASCII
text are allowed. Names may repeat and never replace persistent entity identity.
The existing Core path/log validation and Name reuse one private UTF-8 decoder.

## Math and local transforms

[math.hpp](../engine/core/include/shark/math.hpp) is the only engine GLM include
point. The public SharkMath CMake interface selects radians, right-handed math,
zero-to-one depth, explicit constructors, initialized defaults and pure scalar
operations. The wrapper rejects incompatible handedness, alignment, intrinsics
or quaternion-layout switches and prior GLM inclusion. GLM is the existing
documented public math exception; EnTT remains private. Headers do not require a
GPU, C compiler or upstream testbed configuration.

[Transform](../engine/core/include/shark/transform.hpp) stores double-precision
translation in meters, a quaternion rotation and dimensionless scale. Defaults
are zero translation, identity rotation and unit scale. Spatial conventions remain
right-handed, +Y up, -Z forward, column vectors and column-major matrices:
`local = T * R * S`, `world = parent_world * local`. GLM quaternion construction
uses `(w,x,y,z)`; the future serialized convention remains `[x,y,z,w]`. Raw GLM
object memory is never a persistence or protocol format.

Inputs must be finite. Every local scale component has absolute magnitude at
least `1e-6`; negative and nonuniform scale are allowed. Finite nonzero quaternions
are normalized without overflowing or losing subnormal inputs, then canonicalized
to a deterministic sign and positive zero. Local TRS has no shear. A hierarchy
may produce a sheared world matrix, which remains a full matrix.

Matrices must be finite and exactly affine in the last row `[0,0,0,1]`. The absolute
determinant of the independently normalized basis columns must exceed `1e-12`;
singular or numerically unresolved bases fail. Decomposition additionally requires
orthogonality within `1e-9`, scale bounds and successful recomposition. Equivalent
matrix elements use `abs(a-b) <= 1e-9 + 1e-9 * max(abs(a),abs(b))`. Translation never
provides a tolerance allowance for linear terms. Odd reflections place the negative
scale on X. The minimum scale is strict even during decomposition: a composed
matrix at the threshold can round below it and make keep-world conservatively
reject without changing the scene. There is no epsilon snap. Signed TRS decompositions are not unique, so keep-world reparenting
may change the individual scale signs while preserving the matrix.

The scene's world matrix is in its common **local frame**, not a global MMORPG
position. [WorldPosition](scene-contracts.md#global-cell-coordinates) remains a
separate world/cell/double-offset value. M05 owns anchored roots, bounded render
and physics origins, streaming and distributed region handoff. Large finite local
values are not an extent or capacity guarantee; operations reject nonfinite or
unresolved descendant results before publication.

## Hierarchy and mutation

`EntitySeed` in [scene.hpp](../engine/core/include/shark/scene.hpp) adds a Name,
Transform and optional checked parent, retaining the earlier identity/authority
fields. Creation canonicalizes values and validates the parent's scene, lifetime,
authority and depth before adding the entity. Roots have depth one. The default
maximum depth is 256 and the supported hard maximum is 1024. Traversal and subtree
deletion are iterative; depth limits do not depend on the C++ call stack.

`entity_state` returns an owned copy of reference, Name, local Transform, parent,
ordered children, world matrix and depth. Child order is ascending runtime slot;
it is not an authored sibling-order format. Changes update all affected cached
world matrices synchronously. The returned copy survives subsequent mutation but
its embedded checked reference may become stale. 07c's [bounded revisioned
semantic queries](scene-inspection.md) provide explicit field selection and
immutable pages when a full per-entity state copy is unnecessary.

`set_name`, `set_transform` and `reparent` follow the existing immediate mutation
boundary: no active visit and no pending deferred work. Successful changes advance
the scene revision once; exact no-ops do not. Reparenting rejects stale/foreign
parents, self-parenting and ancestor cycles. Replica children and parents must have
the same source authority stamp; a local transform link cannot cross an authority
incarnation. This check is metadata validation, not network authentication.

- **keep-local** preserves the local TRS and recomputes the affected world matrices.
- **keep-world** computes the new local matrix from the inverse parent world matrix,
  rejects nonrepresentable local TRS, and checks every affected world matrix against
  its previous value using the fixed tolerance before committing.
- **destroy** removes the entire subtree atomically and advances the revision once.
  To preserve children, explicitly reparent them before destroying their parent.

Every prospective subtree is validated before any component or parent/child link
changes. Depth overflow, matrix failure or invalid input leaves state and revision
unchanged. Name/Transform cannot be removed from an entity. Revision/handle
exhaustion still rejects or retires storage without wrapping.

The matching `defer_set_name`, `defer_set_transform` and `defer_reparent` operations
own their inputs and use the existing bounded FIFO queue. Local values and current
references are checked at enqueue; hierarchy, references and composed results are
checked again when applied. A preceding operation can make a later request stale,
cyclic or numerically invalid. Each gets its own outcome and later valid operations
continue. Destroying a parent makes a later queued child mutation stale. Failed
visitors cancel their newly queued edits as in 07a. This queue is not 09's atomic
multi-operation authoring transaction.

## Qualification

`shark.core.hierarchy` tests Name validation, owned state, parent/child consistency,
cycles, subtree deletion, depth limits, matrix propagation, both reparent policies,
failure rollback, deferred order and owner-thread access. `shark.core.transform`
tests conventions and numerical rejection independently of the registry. Existing
scene lifetime, coordinate, log/path and dependency/package tests remain active.
See the [07b handoff](handoffs/07b-transform-hierarchy.md) for executed results.

The two components have implemented C++ validation, mutation and schema/query
coverage. Their version-1 schemas expose required/nonremovable components,
typed defaults and scene-owner mutability. Transform's quaternion metadata is
explicitly `[x,y,z,w]`; accepted scale uses an absolute magnitude floor, preserving
negative/nonuniform scale. Pure schema validation reuses `validate_name` and
`canonicalize_transform`; Scene still checks world-matrix and hierarchy feasibility.
The [07c handoff](handoffs/07c-schema-inspection.md) and
[receipt](coverage/evidence/07c-schema-inspection.json) record passing
current-machine qualification. 07c and full REQ-07 are PASS. M02 is active;
its [transport foundation](handoffs/M02a-transport-wire.md) is complete and
[M02b owned sessions](handoffs/M02b-owned-sessions.md) is **PASS** under the
current-machine scope. Session readiness grants no Transform mutation or gameplay
authority. M02c replication is next.
Assignment 08 supplies persistence, 09 transactions,
10 FeatureWorld, 15 terminal adapters, 16/17 editor fields and 19 selected Lua APIs.
Those later integrations are planned fixtures, not claimed passes.
