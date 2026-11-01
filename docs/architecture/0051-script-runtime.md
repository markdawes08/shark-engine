# ADR 0051: Bounded checked script lifecycle

Date: 2026-11-01. Status: accepted for checkpoint 19c.

Introduce a uniquely owned, creating-thread Core ScriptRuntime with trusted
bounded source snapshots, isolated per-entity Lua VMs, deterministic lifecycle
order, an exclusive weak Scene lease, explicit teardown and no retained Scene borrow.
Pending deferred mutations prevent teardown callbacks until drained. Use checked entity
references and existing scheduler callbacks; do not expose native handles or
create a second clock/job system. Faulted callbacks do not imply rollback.

Server gameplay requires a Region Scene; presentation runs only on Authored or
Replica Scenes. Only the server fixed-update phase may use the current Name
mutation; all other phases/profiles remain read-only, and existing Scene/physics
leases can reject writes. Region scope metadata alone is not authentication.
Expose the small [versioned API](../script-runtime.md), including bounded logs,
time and math, without claiming hostile-code sandboxing or production capacity.

Source acquisition, cooked dependency closure, integrated editor/terminal run,
client and Server runtime activation and full assignment acceptance retain 19d
or their later owners. Existing export and activation rejections remain until
those owners can honor script content.
