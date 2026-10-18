# ADR 0010: Development and acceptance on the available PC

- Date: 2026-10-17
- Status: accepted user-directed scope amendment
- Reviewed base: `2ef281b` on `main`, initially clean
- Owners: all remaining assignments; 02 feasibility, 14/31/35 packaging, M02-M10 local MMO acceptance

## Decision

The user instructed: "adjust the gameplan for this engine to be limited to what
is currently available on this machine" after confirming that only this Windows
PC is available. Required development, acceptance and packaging now use this PC
and its existing WSL2 environment. No additional computer, GPU, cloud deployment,
paid service, signing certificate, remote account or hosted runner is a
prerequisite for completing the roadmap.

Keep the seamless open-world MMORPG architecture and equal human/editor and
terminal-agent workflows. Develop and qualify a local multiplayer engine and
reference world at a bounded workload. This scope does not promise a public MMO
service, production concurrency, high availability or compatibility with other
machines. Future dependencies and development tools can be introduced locally by
their owning assignments with the existing pinning/license rules; the scope does
not freeze development to already implemented features or require an LLM to run
on the local GPU.

This decision supersedes conflicting platform, independent-machine, hosted-gate,
capacity and public-release obligations in ADRs 0003/0006/0007/0009 and the source
roadmap. Mac remains outside scope. Original assignment IDs and order remain.

## Available baseline

Inventory captured on 2026-10-18 UTC (2026-10-17 local):

| Resource | Observed baseline and use |
| --- | --- |
| Native host | Windows 11 Home x64, build 26200; editor/client, graphical authoring and exports |
| CPU / RAM | Intel Core i9-14900HX, 24 cores / 32 logical processors; 34,053,414,912 reported physical bytes, approximately 32 GB |
| Primary GPU | NVIDIA RTX 4070 Laptop, 8,188 MiB reported VRAM, driver 610.62, Vulkan 1.4.341; primary rendering/performance profile |
| Secondary GPU | Intel UHD / Vulkan Raptor Lake-S Mobile, driver 101.6790, Vulkan 1.4.311; capability rejection, correctness and reduced-quality fallback |
| Linux userspace | Existing Ubuntu WSL2 host and isolated pinned Ubuntu 24.04 userspaces; CPU tests, headless Server, persistence and package fixtures |
| Storage | Local C: volume, approximately 524 GiB total and 116 GiB free at inventory; free space is time-dependent, not a reserved project budget |

GPU UUIDs and full tested identities are in [ADR 0003](0003-platforms.md) and the
[02d Windows receipt](../coverage/evidence/02d-windows-refresh.json). Existing
MSVC/Windows SDK and pinned build tools remain the Windows build route; existing
GCC/Clang userspaces provide Linux CPU/sanitizer coverage. WSL is not native Linux
graphics, and the local development machine is not an independent clean machine.
Do not alter WSL resource settings or the host to manufacture capacity results.

## Required targets and gates

1. **Windows editor/client:** qualify the two actual GPU rows `LOCAL-W-NV` and
   `LOCAL-W-INTEL`. Retain required Vulkan/NVRHI validation, analytic readbacks,
   comparison tolerances and the 100 resize / 20 device / 600-second feasibility
   workload. Run separate qualification jobs and GPU profiles serially; the
   explicit multiplayer functional fixture may run its two graphical clients
   together. Do not infer equal performance from
   correctness on both GPUs; assignment 34 measures the primary profile.
2. **Local headless Server:** qualify Linux CPU/server behavior in the existing
   WSL2 Ubuntu environment. Keep presentation dependencies outside Core/Server.
   Use multiple local processes for client/server and region boundaries, with
   bounded deterministic network-fault injection. No second physical server,
   independent network, public identity provider or remote database is required.
3. **Portable local packages:** inspect actual dependency closure, then launch
   relocated Windows packages outside the checkout with restricted environment
   paths, standard-user execution and Unicode/space paths. Use separate SDK-free
   Ubuntu userspaces for headless/package closure. Both routes retain missing,
   corrupted, incompatible-content and cancellation/recovery fixtures. These are
   the required local package gates; never relabel them independent-machine tests.
4. **Local MMO acceptance:** use the explicit `LOCAL-MMO-1` profile in
   [ADR 0007](0007-mmorpg-architecture.md): 16 active synthetic players across two
   region processes, 16 NPCs, 32 visible animated actors, two graphical clients in
   a separate semantic fixture, 100 region crossings and a two-hour active soak.
   These are planned tests, not measured capacity. Preserve authoritative state,
   fencing, idempotency, persistence, authentication/authorization and recovery
   correctness; model partitions, loss and crashes locally. Measure and bound
   resource use before qualifying M06/M10; report host contention and overload.
5. **Human/agent acceptance:** use the available Windows desktop and existing
   terminal agent on the same project. CPU authoring uses the shared application
   services without a display requirement. GPU/audio checks use actual available
   devices as those features land; do not introduce a new hardware matrix.
6. **Local release candidate:** assignment 35 produces documented local Windows
   editor/client and WSL-compatible Server packages, tested samples, notices,
   archived inputs/symbols and recovery evidence for this baseline. Local test
   keys can exercise content verification and rejection of tampering; public
   certificate-backed signing, stores, online hosting, independent hardware and
   production operations are outside this release scope. Test keys must not be
   presented as production trust and private keys must stay out of packages/Git.

Native Linux graphical clients/editor, the former six baseline GPU rows,
independent physical clean-machine tests, real WAN/multi-host qualification and
large-population production capacity are removed from mandatory scope. Existing
Linux graphics builders/fixtures can remain for CPU/package regression; their
presence creates no native Linux graphical support commitment. Any later scope
expansion needs explicit acceptance criteria and evidence before a support claim.

## Evidence and assignment progression

The original six GPU rows remain historically NOT_RUN; they are retired by this
decision, not retrospectively passed. The existing immutable 02d receipts prove
both local GPUs completed the full workload with zero validation warnings/errors,
Windows package relocation and failure cleanup, and Ubuntu build/CPU/SDK-free
package behavior. Native P02-07/P02-08 fields stay unchanged: package/failure
acceptance is aggregated from their separate fixtures under this amended scope.

**Assignment 02 is complete for this PC under the amended gate. Assignment 03 is
next and has not started.** This decision adds no renderer, editor UI, networking
or gameplay implementation. The earlier resize timeout remains preserved and
unexplained; new scope does not turn it into a fixed bug. New failures on the
active baseline still require investigation and honest results.

Local builds, tests, sanitizers, dependency boundaries and applicable hardware
fixtures determine roadmap progression. Existing nine-job GitHub CI is retained
as supplemental regression evidence; its configured branch protection is
unchanged by this planning amendment. An unavailable hosted run does not block
local roadmap completion, and a failed remote run is never called PASS. Actual
failures affecting in-scope behavior still need investigation. The user continues
to own staging, committing and pushing; do not change remote policy implicitly.

See the [scope handoff](../handoffs/current-machine-scope-amendment.md) and
[scope receipt](../coverage/evidence/current-machine-scope.json) for inventory,
unchanged evidence hashes, the existing-code assessment and validation.
