# Prepared scenes in Runtime

Assignment 13e connects the [prepared content](prepared-content.md),
[extraction](scene-rendering.md) and [viewport](scene-viewport.md) owners to Runtime.
Runtime reads explicit bounded engine payloads and precompiled scene shaders; it
does not import source documents/glTF or discover an editor/compiler installation.
14b adds [standalone export and package-relative startup](standalone-export.md);
[14c](handoffs/14c-isolated-export-acceptance.md) qualifies isolated package
acceptance; 31 retains final cooking.

`SceneContentFiles` names one scene and at most 126 mesh files. The loader admits
regular files only, existing per-format limits and a combined 32 MiB input budget.
Embedded asset IDs, duplicate/reserved identities, checksums and every scene
reference are validated before a fresh `LoadedPreparedScene` is returned. Failure
leaves an existing owner untouched. File snapshots are explicit development inputs,
not a coherent multi-file publisher; checksums/references detect invalid mixtures,
and this explicit-file mode does not supply export generation consistency.
The package mode validates the startup descriptor's exact file sizes/digests
and decodes those same snapshots before replacing a complete scene/shader owner.

`Runtime --scene PATH --camera UUID --vertex-shader PATH --pixel-shader PATH`
accepts repeated `--mesh PATH`, `--device UUID|auto`, `--validation required|off`,
`--extent WIDTHxHEIGHT`, `--scene-ms 1..60000`, `--frames 1..600` and
`--capture-directory PATH`. Defaults are a 640x480 window and validation off.
Required validation is used for qualification. Help/argument rejection and content
loading do not initialize a device. Camera selection is explicit; no implicit
first-camera ordering or alteration of authored content occurs.

The main thread owns content, the existing offline event pump and GraphicsDevice.
Resize repacks a revisioned snapshot for the actual framebuffer. Zero-sized or
iconified windows suspend presentation. F5 prepares a complete replacement from
the same files; failed reads/validation retain the last valid scene. A successful
replacement gets a fresh Scene incarnation. F6/F7 retain offline pause/step; these
controls never affect a server or grant gameplay authority.

`set_presentation_scene` owns the packed request, drains previous submitted work
before replacement and preserves failed-completion retention. It is exclusive of
the fixed cube draw. Scene geometry shares the offscreen implementation while
the existing swapchain owns presentation. Framebuffer/request extents must match;
scene dimensions remain bounded to 2048. The sRGB attachment encodes RGB once;
alpha stays linear. Temporary opaque unlit vertex color times base-color factor
has no textures, PBR, lighting, cutout or blended transparency.

The requested extent is a native window content size. Platform minimums or DPI
can change the actual framebuffer; rendering/capture report the actual extent.
A frame-bounded run has a 60-second maximum wait unless a shorter duration is
specified, and fails if it closes or times out before the requested frames.

Capture requires a fresh output directory and an actually presented frame.
`color.rgba` contains tightly packed RGBA8 sRGB/linear-alpha; `depth.f64` contains
little-endian normalized depth doubles converted from D32. `report.json` and the
completion JSON line identify dimensions, adapter, validation, scene revision,
draw count and safely closed presentation counters. A nonzero exit reports failure;
partial files are never a completed capture. Interactive completion is not a
hardware qualification claim. The independent acceptance supervisor checks images
and actual inputs separately. No source project or mesh file is written by Runtime.
