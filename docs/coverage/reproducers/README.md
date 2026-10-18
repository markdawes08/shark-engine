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

## ASan process-exit capture

The [ASan diagnostic](05c-asan-exit.py), [scoped capture helper](05c-exit-capture.cpp)
and [late exit markers](05c-exit-markers.hpp) investigate the retained Intel ASan
timeout after the fixture emitted successful native cleanup. They build separate
diagnostic copies in ignored output using the existing `windows-asan` libraries;
they do not alter the normal fixture, engine, host settings or driver.

Run the device-free observer selfcheck first, choosing a fresh output directory:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','docs/coverage/reproducers/05c-asan-exit.py','--output','out/asan-exit-selfcheck')
```

Then run the fixed native sequence:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','docs/coverage/reproducers/05c-asan-exit.py','--output','out/asan-exit-native','--mode','native')
```

An existing matching ASan build and required validation tools are prerequisites.
`--include-prior` additionally requires the locally retained original fixture and
executable. `--source-revision <commit-id>` snapshots an immutable Git revision
when its unchanged binaries must be investigated while separate source work
proceeds; its computed source digest must match the existing Ninja metadata.
Omitting this option snapshots the current source. The retained investigation used
`--include-prior --source-revision 9613bec` before the separate queue-wait fix was
built. Do not use that old revision with a newer build.

Each of eight blocks runs NVIDIA's four faults, then Intel's four faults, matching
the immediate recorded original adapter/fault order. Original and instrumented
variants alternate; optional prior variants give sixteen children per variant.
The limit is 64 children. Each keeps the original 30-second deadline. An eight-second
observation threshold captures only the owned native child, Python gate worker and
venv redirector identified by both PID and creation time. The sequence stops on its
first slow capture or unexpected result. Output uses files, so inherited pipe EOF
is not a completion condition.

The separate observer creates a minidump and bounded wait-chain report. If CDB is
installed, `--debugger <path-to-cdb.exe>` selects it explicitly for offline, local
symbol analysis; no debugger is attached to a live graphics child. The CPU
selfcheck deliberately stalls native `atexit` and Python-worker shutdown, proves
the original finite deadline still wins, and rejects a mismatched process
creation time. Only newly added late marker functions omit ASan instrumentation;
the copied fixture and linked engine retain their existing instrumentation.
The EXE TLS marker does not prove all DLL detach callbacks have finished.

The [ASan investigation receipt](../evidence/05c-asan-exit-investigation.json)
records 64 expected finite exits with no reproduced hang. That result remains
`UNRESOLVED` / `qualification: NOT_RUN`; successful repetitions and the separate
queue-wait fix do not explain or erase the original post-cleanup timeout.

## Intel USER hook lifetime investigation

The [owned-hook control](05c-intel-user.cpp) and [runner](05c-intel-user.py) isolate
hook cleanup from the engine. Both modes are diagnostic only and retain
`qualification: NOT_RUN`. Choose a fresh output directory for each invocation:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','docs/coverage/reproducers/05c-intel-user.py','--output','out/intel-hook-control','--mode','hook-control')
& ./tools/windows_dev.ps1 -CommandArguments @('python','docs/coverage/reproducers/05c-intel-user.py','--output','out/intel-raw40','--mode','raw40')
```

`hook-control` links only Windows USER APIs and the C++ runtime. Twenty owned
thread lifetimes alternate removing hook IDs 4/3/12 before thread exit and trying
removal after joining the target thread. The control never touches another
component's handles. It records each return/error and USER/GDI counts, then pumps
messages for fixed observations through five seconds. A 30-second process limit
bounds the whole experiment. Default/help executable invocations create no hooks.

`raw40` generates an exact, checked forty-lifetime variation of the existing raw
Win32/Vulkan source and reuses its strict trace parser and eight corruption checks.
It selects the current Intel UUID with required core/synchronization validation,
links the pinned Vulkan loader, and performs no acquire/submit/present work. It
contains no Shark, GLFW or NVRHI libraries. Its process deadline is 120 seconds;
`--build-only` compiles without starting the diagnostic. The final generated source
was verified byte-for-byte against the independently executed forty-lifetime
reproducer; the newly built raw executable was not separately GPU-tested.

Optional `--debugger <installed-cdb.exe> --symbols <existing-local-symbol-cache>`
adds supported USER API, `CreateThread` and `RtlExitUserThread` breakpoints. The
cache must contain matching Microsoft NTDLL type information; the runner does not
download symbols or change host configuration. It pairs API entry/return by
thread, stack and return address and rejects ambiguous traces. Raw stacks, module
loads and target-thread IDs remain available for attribution. A thread-exit entry
does not itself establish completed termination, and debugger pauses change
timing. Symbol files and generated debugger commands are hashed with the inputs.

The [Intel investigation receipt](../evidence/05c-intel-user-investigation.json)
binds the original full transition and drain controls, the raw forty-lifetime
trace, and the owned-hook control. The raw trace recorded eleven failed Intel
hook removals after their target driver threads entered exit, with eleven
additional USER objects before final DLL teardown. Six later failures are
recorded separately. The independent control retained three USER objects for
each post-exit triplet but none for removals completed while targets were alive.
This identifies an external hook lifetime problem on the current driver/Windows
combination. It does not authorize foreign-handle cleanup or relax the 05c gate.
