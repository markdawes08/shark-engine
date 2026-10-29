# Dear ImGui dependency review

Owner: assignment 16a / native Editor only. Reviewed 2026-10-29.
The official [v1.91.9b release](https://github.com/ocornut/imgui/releases/tag/v1.91.9b)
resolves to commit `f5befd2d29e66809cd1110a152e375a7f1981f06`.
This fixed-atlas API is used deliberately for the initial shell. The
[upstream integration guide](https://github.com/ocornut/imgui/wiki/Getting-Started)
describes the core/platform/renderer split; Shark supplies its own two adapters.

The immutable archive is 1,846,673 bytes, SHA-256
`85f4ce357df05bcc331b587f01976f47fb55f19fadf477a7907289686bc3f4c8`.
[dependencies.lock.json](../../dependencies.lock.json) pins the URL, commit,
reviewed source hashes, 243 regular files, 6,967,002 expanded bytes, license
inputs and integration options. No patch or external transitive download exists.
The canonical archive is `.tools/dependency-archives/imgui-<commit>.tar.gz`;
`SHARK_IMGUI_ARCHIVE` can supply that same exact archive explicitly.

`cmake/EditorDependencies.cmake` creates static `SharkImGui` / `Shark::ImGui`
only when both Editor and native graphics are enabled. It compiles `imgui.cpp`,
`imgui_draw.cpp`, `imgui_tables.cpp` and `imgui_widgets.cpp`, with SYSTEM headers
and the selected sanitizer flags. Engine warning policy is not imposed on vendor
sources. Upstream backends, examples, demo, build scripts, docking, multi-viewport
and FreeType are unused. ImGuizmo remains assignment 17.

Reviewed definitions disable upstream file I/O, shell-opening and Win32 default
IME hooks. The shell also sets ImGui ini/log filenames to null. Clipboard support
remains upstream native behavior; Unicode input arrives through Shark. No source
patch, global flag or custom `imconfig.h` is applied.

The MIT license is selected, including the MIT option in bundled stb helpers.
The complete upstream `LICENSE.txt`, `imgui_draw.cpp` ProggyClean attribution
and three `imstb_*` headers are retained as license inputs. Any future Editor
distribution must retain these notices; Runtime and Server do not distribute
this dependency. No authoring-tool packaging claim is made in 16a.

The bounded source helper verifies the exact archive, safe regular-file inventory
and complete populated tree before use. Encoded/expanded bounds are 2/8 MiB,
with at most 256 files and 384 members. Downloads are bounded to the pinned byte
count plus one and publish only verified bytes. Offline configurations require
the exact cache. Source overrides, target collisions and mutated populations
reject; Editor-OFF and CPU Editor configurations ignore these irrelevant inputs.
Pin review and CPU dependency checks do not qualify native GUI rendering.
