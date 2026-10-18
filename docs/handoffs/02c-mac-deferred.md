# 02c native Mac work deferred

Historical scope note: the later [Windows/Linux amendment](windows-linux-scope-amendment.md)
removes all Mac compatibility requirements and retires 02c. The deferral below
is historical; no Mac work or hardware qualification remains required.

Base revision: `b6aabd7` (`feat(graphics): add validated GPU cases and lifecycle probes`).
Decision recorded 2026-10-18 UTC / 2026-10-17 America/New_York. The user is on
Windows and instructed: "if this means you cant create the mac portion then skip".
Native Mac compilation and GPU verification cannot be completed on this host.
02c is therefore deferred, with its results **NOT_RUN**. No Mac target is removed,
no replacement renderer is selected, and no untested Mac dependency graph is
added to the lock. The next available work is Windows 02d, within assignment 02.

The existing GLFW surface/portability setup is retained. Source inspection found
no reason to change it speculatively. The offline shader preparer currently pins
Windows/Linux DXC binaries; the validation preparer also lacks a native Mac route.
Neither helper is evidence of a Mac build or supported Metal translation path.

Before resuming 02c, obtain Apple Silicon/macOS 26 and Intel Mac hosts covering
both Intel and AMD graphics, then pin and build the complete tested input graphs.
KosmicKrisp remains the Apple Silicon candidate. Its native Metal 4 integration,
LLVM/Clang and SPIR-V translation toolchain require native evidence; a non-Mac
stub build cannot qualify it. See the [Mesa driver documentation](https://docs.mesa3d.org/drivers/kosmickrisp.html).
MoltenVK remains the Intel candidate. Its public-API distribution, source fetch
inputs and source-archive version generation require review before pinning;
the private-API artifact is not an interchangeable shipping choice. See the
[MoltenVK release](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2)
and [runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/db66022459ffb663aa2b50f6b018bc2e124f5edf/Docs/MoltenVK_Runtime_UserGuide.md).
DXC needs its separately pinned native source/submodule/toolchain route; see its
[build instructions](https://github.com/microsoft/DirectXShaderCompiler/blob/01b62ad47db7dee3dd33f90e7339cf9e860b3f93/docs/BuildingAndTestingDXC.rst).
These are research leads, not downloaded, built or qualified Mac inputs.

All three named Mac GPU rows remain open under [ADR 0003](../architecture/0003-platforms.md).
Windows packaging and supplemental local GPUs cannot replace them. Assignment
03 and dependent production renderer work remain gated by assignment 02.
