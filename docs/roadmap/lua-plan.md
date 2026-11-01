# Assignment 19: Lua scripting checkpoints

Base: `16a0f63`, initially clean `main`. REQ-19 remains open until the
connected assignment acceptance is complete. These checkpoints follow the
[testing policy](../testing-policy.md), not repeated full-project matrices.

| Checkpoint | Scope |
| --- | --- |
| 19a | Pin and privately integrate Lua; implement a bounded, owner-thread VM with controlled libraries, protected execution and useful errors. Verify portable CPU behavior, sanitizers and affected dependency boundaries. |
| 19b | Add Script component, persistence and shared human/terminal authoring using the existing retained owner and history. |
| 19c | Connect ordered lifecycle callbacks and a small checked entity/log/math/time API, separating server gameplay from client presentation; maintain the Lua manifest and FeatureWorld coverage. |
| 19d | Qualify diagnostics, runaway recovery, editing and execution through connected human and terminal workflows, with affected subsystem closure. |

19b records shared Script settings, prepared v4 compatibility and explicit
Runtime/export rejection. The [19c handoff](../handoffs/19c-script-runtime.md)
records the Core lifecycle and checked server/client API. Source acquisition,
cooked closure and connected product execution remain 19d or later; full REQ-19
remains NOT_RUN.

Physics-specific scripting, queries and contact callbacks belong to assignment 20.
No player connection gains script upload or arbitrary server execution authority;
render-frame code cannot grant authoritative gameplay outcomes. Source remains
text, native Lua values stay private, and this VM is not a complete hostile-code
sandbox. One existing scheduler/Scene owner remains authoritative.
