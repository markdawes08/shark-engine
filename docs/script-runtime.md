# Script runtime and Lua API v1

19c adds [ScriptRuntime](../engine/core/include/shark/script_runtime.hpp), a
creating-thread Core owner over private Lua VMs. The caller supplies owned,
bounded source snapshots for enabled Script references in an existing Scene.
Core does no file I/O: the caller must establish ProjectFiles containment,
typed-path exclusion and source freshness before constructing snapshots. There
is no player script-upload endpoint or new scheduler. Runtime/export still
reject Script-bearing scenes until cooked source closure and product integration
are implemented. The terminal `scene.run` remains a static integrity runner.

## Lifecycle and limits

Creation captures enabled Script references and compiles their source in stable
entity-identity order, with one VM per entity. Each VM exposes optional globals
`on_init`, `on_update`, `on_fixed_update` and `on_destroy`; a missing callback
is a no-op, and a non-function callback fails. The owner calls `start`, `update`,
`fixed_update` and `stop` explicitly on the creating thread and supplies the
same Scene each time. A weak Scene lease admits only one ScriptRuntime owner;
it expires on stop, close, destruction or failed creation without retaining a
Scene pointer. The lease is separate from the physics write lease. Enabled Script
identity, generation, scope and settings are rechecked at each pass; changes
require a new owner. Start and update run in stable identity order; stop attempts
initialized callbacks in reverse order. A stale or changed entity is not called
at teardown: it is counted as discarded with an owned contextual error. Explicit
`stop` rejects while deferred Scene mutations are pending, without consuming
callbacks; the host can drain and retry, or use `close` to discard remaining
callbacks and report their count. Logs remain drainable after stop or close.

The host calls fixed updates from an existing scheduler. The first tick may be
zero; subsequent ticks must be consecutive and fit a signed 64-bit Lua integer.
Host-supplied deltas must be finite, between zero and one second inclusive. A
callback failure faults the owner and stops the pass. Earlier effects and Lua
state are not rolled back; explicit stop still attempts eligible teardown and
reports failures. A failed load does not publish a runtime. New Script components
or changed source/settings are not silently adopted into a live owner.

At most 64 source snapshots of 32 KiB each and 2 MiB total are admitted. Configured
instance count is at most 64; total possible VM memory is at most 64 MiB and the
sum of per-pass instruction budgets at most ten million. Default configuration
allows 16 VMs, 4 MiB each and 100,000 instructions per call. Up to 256 owned log
entries and 64 KiB of message text may be queued; each nonempty valid UTF-8 log
message is at most 256 bytes without controls. A full log sink fails the current
callback. As in [19a](lua-vm.md), the instruction hook does not meter native
library work or guarantee a wall-clock deadline; this is not a hostile-code
sandbox.

## Authority and API

The host explicitly chooses a profile. `server_gameplay` requires a Region Scene;
`client_presentation` accepts an Authored or Replica Scene and cannot attach to a
Region Scene. A Scene scope is not itself proof of authentication or distributed
ownership: the trusted host must own the actual region and authority fence.
Client scripts cannot write Scene state. Server scripts may change the checked
self Name only in `on_fixed_update`; load, init, frame update and destroy cannot
write it. Existing Scene write/physics leases remain authoritative and can
reject the operation. No binding exposes native registry pointers, physics,
networking, inventory, currency or arbitrary file access.

Every script starts with `shark.api_version == 1`. This is ordinary mutable
per-VM Lua data for discovery, not an authority token. The implemented functions
have exact argument counts and types:

| Entry | Behavior |
| --- | --- |
| `shark.entity.name()` | Read checked self Name. |
| `shark.entity.position()` | Return self's local translation as x, y, z; not a global cell position. |
| `shark.entity.camera_near()` | Return checked Camera near clip; fail if absent. |
| `shark.entity.set_name(text)` | Set a valid Name only in server fixed update. |
| `shark.time.tick()` | Return the host's current fixed tick. |
| `shark.time.delta()` | Return the current host-supplied delta in seconds. |
| `shark.log.info(text)` | Append a bounded owned log entry. |
| `shark.math.clamp(value, minimum, maximum)` | Clamp finite numbers; reject invalid types, nonfinite values and reversed ranges. |

Each entity operation resolves its owned generational reference again and never
retains component pointers. API and callback errors include script/entity context.
The [coverage manifest](coverage/requirements.json) maps these entries to their
valid/invalid fixtures and a real Lua exercise in FeatureWorld.
