# Private Lua VM foundation

19a provides a private EngineCore primitive for later scripting integration.
It does not expose a gameplay API, Scene access, a terminal command or an Editor
control. Lua 5.5.1 is compiled as C from the locked upstream archive; its native
types and headers do not form part of Shark's public interfaces. See the
[checkpoints](roadmap/lua-plan.md) and [handoff](handoffs/19a-lua-foundation.md).

Each VM belongs to its creating thread. Foreign execution rejects; a live owner
must be destroyed on that thread. Source is UTF-8 text only, up to 256 KiB,
with a nonempty bounded chunk name. A private allocator accounts for the native
allocation and its header; configured limits are 64 KiB to 64 MiB, with a 4 MiB
default. Each execution receives a fresh instruction budget, default 100,000
and bounded to 1 through 10,000,000. The protected call reports syntax, runtime,
memory and instruction errors with bounded owned diagnostics. Runtime failures
do not roll back assignments already made by the script. VM state is isolated
between owners, and a later call can run after a recoverable failure.

The initial profile opens a restricted base library plus table, string, math and
UTF-8 helpers. It removes host file access, dynamic loading, printing, random,
bytecode dumping, metatable access, protected calls and explicit collection;
IO, OS, package, debug and coroutine libraries are absent. These restrictions
prevent scripts from catching the instruction-limit exception or installing
script finalizers during teardown. The planned coroutine capability requires a
separate bounded design before the full v1 profile is complete.

The instruction counter measures Lua VM instructions, **not wall time or work
inside native library calls**. Pattern matching and other library operations can
consume disproportionate CPU before the hook resumes. The memory and source
limits do not resolve that limitation. This is a foundation for trusted local
project code, not a complete hostile-code sandbox, a real-time deadline or a
production player-script admission mechanism. Project editing, callback ordering,
checked handles, role-specific capabilities, gameplay and UI recovery remain
with later checkpoints.
