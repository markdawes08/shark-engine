# Lua physics and contact delivery

20c connects the existing [physics controls](physics-controls.md), [queries](physics-queries.md), [contact queue](physics-contacts.md) and [script runtime](script-runtime.md). A trusted host supplies the same Scene and PhysicsWorld to each protected lifecycle call; neither is retained across calls. Client presentation can read a supplied world, but only a trusted server gameplay host can mutate physics during fixed update or contact delivery. A Region scope is a necessary check, not proof of network authority. No player command or server gameplay loop is added.

## Lua surface

All body operations address the script's own checked entity and its current native body. Numbers must be finite and valid for the underlying Core operation; argument count and types are checked. Velocities and coordinates use bounded scene axes.

- `shark.physics.velocity()` returns six numbers: linear x/y/z and angular x/y/z.
- `shark.physics.set_velocity(lx,ly,lz,ax,ay,az)`, `force(x,y,z)`, `impulse(x,y,z)`, `teleport(x,y,z)` and `kinematic_target(x,y,z)` retain the corresponding Core motion, magnitude and lifetime rules. Teleport and target preserve the current rotation and scale.
- `shark.physics.raycast(fx,fy,fz,tx,ty,tz,layer,mask,include_triggers)`, `overlap_sphere(cx,cy,cz,radius,layer,mask,include_triggers)` and `overlap_box(cx,cy,cz,hx,hy,hz,layer,mask,include_triggers)` return complete arrays of at most 32 owned observations or fail on overflow. Each hit has `entity_id` and `is_trigger`; ray hits also carry `fraction`, `x`, `y`, `z` and, when available, `normal_x`, `normal_y`, `normal_z`. Results are observations scoped to the supplied active world, without explicit world, Scene-revision or tick provenance; they are not body handles or mutation authority.
- `shark.physics.remove_body()` and `create_body()` are contact-only deferred requests. Recreation uses captured validated original settings and requires matching removal; it does not create arbitrary Scene entities.

All physics calls fail if a host did not provide the world or checked ownership is unavailable. Calling a mutation from client presentation, load, init, update or destroy is rejected. Lua instruction limits do not bound native query work; the separate query limits still apply.

## Contacts and partial effects

Native contact callbacks collect data only. After a successful fixed step the creating-thread host calls `dispatch_contacts(scene, world, maximum)`, which drains a bounded FIFO prefix and delivers `on_contact_enter(other_uuid, self_trigger, other_trigger, completed_tick)` and `on_contact_exit(...)` in event order. The report owns the original consumed observations, callback counts, applied/discarded command counts and any error. Participants and body generations are checked at delivery. Stale observations remain in the report and are not a grant of authority. Native physics workers never invoke Lua.

Contact-phase writes are queued in callback order, bounded to 64, and applied only after the callback batch. Each command rechecks its ownership and generations. Queue overflow, a failed callback or failed application faults the script owner; previously completed callbacks, native steps and applied commands are not rolled back. Removed-body exits remain in the world FIFO and the host must drain them before admitting the next native step. No consumed callback is blindly replayed.

Editor and terminal preview use the same owner and dispatch path. Their client presentation profile cannot author authoritative gameplay physics. The retained operation receipt includes completed events and script logs even if a callback later faults; exact retries return the original receipt without invoking callbacks again. Source edits and history remain isolated. Connected assignment acceptance and debug presentation remain later [checkpoints](roadmap/physics-interaction-plan.md).
