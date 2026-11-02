# ADR 0055: Lua physics and owner-thread contact delivery

Date: 2026-11-01. Status: accepted for checkpoint 20c.

Connect Lua to the existing checked Scene and PhysicsWorld owners through a temporary private protected-call bridge. Publish bounded owned query observations rather than native handles. Retain server gameplay versus client presentation capability checks; trusted host admission remains necessary and Region metadata is not authentication.

Deliver bounded native contact observations explicitly on the creating thread after simulation commits. Native callbacks never execute Lua. Queue contact-phase physics changes in bounded order until the complete callback batch has run, then recheck participant generations and apply through the existing owner. Allow self-body recreation only from captured validated settings after matching removal. Consumed events, completed native work and applied commands remain explicit if later work fails; no rollback or blind callback replay is promised. Owner-driven dispatch drains removal exits separately before another step. Editor and terminal preview share this path and retain their exact operation receipts.

This changes public Core and Lua APIs and extends preview log phases; it changes no source, prepared, gameplay wire or database format. Existing hosts that do not supply PhysicsWorld retain lifecycle behavior; physics functions report unavailable. Debug presentation and connected assignment acceptance remain [later checkpoints](../roadmap/physics-interaction-plan.md).
