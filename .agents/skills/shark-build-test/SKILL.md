---
name: shark-build-test
description: Configure, build and test Shark C++ targets, investigate CTest or sanitizer failures, and verify the runtime dependency boundary.
---

# Shark build and test

Read [AGENTS.md](../../../AGENTS.md), [building](../../../docs/building.md) and
[CI](../../../docs/ci.md). Run from the repository root with the native compiler
environment; Windows Ninja presets need an x64 MSVC developer shell.

Use matching configure/build/test preset names: `windows-debug` or `linux-debug`;
corresponding `-release` variants; `windows-asan` or `linux-asan-ubsan` for sanitizers.
Mac compatibility and CI are outside scope under
[ADR 0009](../../../docs/architecture/0009-windows-linux-platform-scope.md).
The later [current-machine scope](../../../docs/architecture/0010-current-machine-scope.md)
uses native Windows builds and the existing WSL2 Ubuntu CPU/headless environments.
Local checks determine roadmap progression; existing hosted CI is supplemental
and remote branch protection is unchanged. Do not require another computer,
hosted runner or native Linux display to finish an increment.

```text
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
python tools/check_format.py
python tools/validate_contract.py
```

Replace the example preset for the host. `python tools/ci.py --preset
windows-debug --jobs 2` runs the recorded CPU workflow. Inspect the first failed
step and retained evidence before changing code; do not relabel an unavailable
compiler/runner as PASS. List tests with `ctest --preset windows-debug --show-only`
and select real test names/labels. Do not run multiple builds against one build
directory concurrently.

After changes to target dependencies, configure/build `runtime-only` in the host
compiler environment; its ALL boundary target must pass. This preset disables
tests, so a native debug preset must still run CTest. Engine/Core/Client cannot depend
on EngineTools; Runtime cannot acquire authoring or test libraries transitively.
Keep exact dependency inputs in the checked-in lock; do not resolve a newer
floating version as a repair.

For CI failure propagation, use the documented
`python tools/ci.py --preset windows-debug --inject-test-failure` path and expect
a nonzero result. Restore a normal run and record both results. Do not create a
commit or stage files to test CI. This project leaves all staging/commits/pushes
to the user; maintain `.gitignore` and provide suggested commit text.

Report exact command, revision/worktree state, toolchain, platform, outcome and
unverified gates. CPU tests do not qualify GPUs, audio or packaged games. Current
bootstrap executables are not working game/editor applications.

For MMORPG work, read the [execution order](../../../docs/roadmap/mmorpg-plan.md).
Use `server-only` for the no-tests/no-download Release Server package and
`server-debug` / Linux `server-asan-ubsan` for headless CTest. `runtime-only`
does not prove this boundary. The minimal Ubuntu 24.04 command in
[CI](../../../docs/ci.md) checks configure/download and final package closures
as well as links, rejecting presentation dependencies even if client-runtime-safe.
After dependency changes run both isolated builds and the actual server lifecycle
and boundary fixtures. Keep Core/Server free of client and authoring sources.
Add actual server/network/database/load commands only when their owners land;
ordinary bootstrap CTest results are not simulation, capacity or seamlessness evidence.

M02a's private static Mbed TLS integration adds a required C compiler alongside
C++ in Core/Server builds. Prepare the exact EnTT, GLM and complete Mbed TLS
archives through `tools/core_dependencies.py`; disconnected configurations pass
`SHARK_ENTT_ARCHIVE`, `SHARK_GLM_ARCHIVE` and `SHARK_MBEDTLS_ARCHIVE` explicitly.
The headless driver selects matching pinned GCC/Clang C and C++ compilers.
Source closure validates the bundled TF-PSA/framework inputs; package checks
require every locked nested Mbed TLS notice while excluding test credentials
and shared TLS libraries. M02a's local TLS diagnostics do not qualify account
authentication, gameplay authorization, replication or production capacity.

M02b [owned sessions](../../../docs/network-sessions.md) is **PASS** under the
current-machine scope; the [handoff](../../../docs/handoffs/M02b-owned-sessions.md)
records 425 passing CTests and protected package qualification. Follow
[ADR 0012](../../../docs/architecture/0012-owned-sessions.md).
Its `shark.core.net-mtls` and `shark.core.net-session` CTests use seed 1337 and
30-second timeouts; `shark.core.net-sessions-process` has a 90-second timeout.
Use actual mutual TLS peers and controlled monotonic clocks for service tests;
retain separate-process evidence for enrollment, ownership, revocation, expiry,
replay, reconnect and rate-limit behavior. Include public certificate fixtures and
their generator in source-input freeze hashes. Keep test keys and SessionProbe
out of installed packages. Preserve the existing Windows/WSL CPU, sanitizer and
protected package gates when extending these services. M02c replication is PASS
under current-machine scope: see the [replication contract](../../../docs/network-replication.md)
and [handoff](../../../docs/handoffs/M02c-authoritative-replication.md). Its three
additional CTest families use 20/30/90-second timeouts for wire, service and real
process cases; preserve these earlier network families and freeze source/fixture
hashes for full qualification. Session readiness alone does not qualify gameplay or capacity.

M02d [spatial interest](../../../docs/network-interest.md) is locally qualified.
Preserve the complete trusted catalog, authenticated character observer binding,
unthrottled privacy maintenance and bounded managed admission. Its two new CTest
families are `shark.core.net-interest` (30 seconds, seed 1337) and
`shark.core.net-interest-process` (120 seconds); retain these families alongside
the earlier network tests. Two active recipients and controlled reliable-stream
delay do not establish the full LOCAL-MMO-1 workload or WAN behavior. The
[M02d handoff](../../../docs/handoffs/M02d-spatial-interest.md) retains that gate's
historical evidence; M02e subsequently closes integrated qualification.

M02e adds `shark.core.net-adversity` (60 seconds, seed 1337) and
`shark.core.net-adversity-process` (360 seconds), bringing the network prefix to
fourteen CTest families. M02e and full REQ-M02 are PASS under the current-machine
scope in the [handoff](../../../docs/handoffs/M02e-integrated-qualification.md);
assignment 08, Project and scene persistence, is next. Follow the
[qualification contract](../../../docs/network-qualification.md): semantic faults
operate on decoded authenticated state, while the bounded relay preserves TLS
byte order. Record actual injected events, convergence, cleanup, retained vector
capacity and separate process RSS/handle samples. Refresh all earlier network
families and protected packages against the same frozen source inputs.
