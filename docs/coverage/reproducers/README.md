# 05c minimal Win32/Vulkan resource reproducer

This [C++ program](05c-win32-vulkan.cpp) and [bounded runner](05c-win32-vulkan.py)
isolate repeated swapchain creation from Shark, GLFW and NVRHI. They are diagnostic
artifacts, outside the production CMake targets. See the
[investigation handoff](../../handoffs/05c-failure-investigation.md) for the retained
failures, controls and limits.

Use the existing configured/built `windows-debug` preset and locked validation
tools described in [building](../../building.md) and the
[graphics probe guide](../../graphics-probe.md). From the repository root:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','docs/coverage/reproducers/05c-win32-vulkan.py','--output','out/05c-close/raw-vulkan-final')
```

Choose a **new** output directory on each invocation; an existing directory is
rejected to preserve evidence. `--build-dir` and `--validation-manifest` can point
to other prepared local inputs; their defaults are `build/windows-debug` and
`out/05a-work/validation-tools/summary.json`. No dependency downloads or host/driver
changes occur. The script compiles with MSVC `/W4 /WX`, links the pinned Vulkan
loader and Windows libraries, copies that loader beside the executable, and runs
both current-PC GPU UUIDs sequentially through the standard-user launcher.

Each native process has a 120-second deadline and performs eight fresh
window/instance/surface/device/swapchain lifetimes. There is no image acquisition,
command submission or presentation. Required core/synchronization validation stays
enabled through teardown. Ordered JSON observations record GUI/kernel counts
before and after swapchain creation and after full cleanup. The runner checks
exact cycle/adapter identity, diagnostics, cleanup, source/tool identities and
token evidence; eight corruptions of each actual trace must be rejected.

Successful execution returns zero with `status: DIAGNOSTIC_ONLY` and
`qualification: NOT_RUN`. Resource growth is deliberately recorded for diagnosis,
not accepted as a passing stress gate. No driver imports are hooked and no
foreign GDI object is inspected, released or deleted by this reproducer.

The recorded run at base `cd97e87` produced NVIDIA closed GDI counts 3 through 10
and Intel GDI counts fixed at 3, with zero validation/cleanup diagnostics. NVIDIA
USER counts stayed at 1; Intel USER counts rose from 10 to 12 at cycle four, then
stayed there. These whole-process observations are not equivalent to the full
production workload or proof of zero resource growth on Intel. Exact inputs,
commands, raw rows and outcomes are in the
[follow-up receipt](../evidence/05c-investigation.json).
