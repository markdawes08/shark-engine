# ADR 0036: Prepared Runtime scene launch and presentation

Date: 2026-10-28. Status: accepted implementation decision for assignment 13e.

Runtime loads the existing bounded prepared mesh/scene formats from explicit
files through EngineClient. Core's byte codecs and immutable ownership remain
unchanged. Source project/glTF readers and importers stay in EngineTools; export
staging and final cooking remain assignments 14 and 31. The
[Runtime scene contract](../runtime-scenes.md) defines limits and launch controls.

An explicit authored camera ID selects the view. The main-thread application
root owns loaded content, the existing offline event pump and GraphicsDevice.
It extracts and packs complete revisioned snapshots on initial load, resize and
explicit F5 reload. Global cell anchors are metadata, never matrix translations.
Failed candidate loading or camera validation retains the current Scene. A
successful reload creates a fresh incarnation and cannot validate old handles.

GraphicsDevice can replace a scene presentation snapshot after its existing
bounded drain proves prior use complete. The fixed cube mode is exclusive. Scene
geometry, pipelines and depth use the same implementation as the offscreen
viewport; per-image attachments belong to the existing swapchain. Capture reads
the actual presented sRGB/D32 attachments. No second device, scheduler, authoring
owner or runtime shader compiler is introduced. Failed completion retains owners.

The launch CLI is a development viewer and bounded capture path, not assignment
15's authoring protocol or assignment 16's editor. Public C++ APIs are reusable by
those later adapters. Temporary unlit materials retain their documented limits.
The acceptance fixtures prepare source through Tools, then launch Runtime with
engine payloads only; they also destroy/reload visible entities and resize through
the public owners. Source formats and saved FeatureWorld identities do not change.
