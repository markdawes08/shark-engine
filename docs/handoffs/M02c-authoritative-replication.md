# M02c: Authoritative replication and resynchronization

Date: 2026-10-18. Starting revision:
`08616c433648d81142801174dbb488ebc15114fa` on `main`, initially clean.
Changes remain uncommitted; the user owns staging, commits and pushes.

**M02c is PASS under the current-machine scope.** All 446 CPU CTests pass with
zero failures/skips, alongside protected builds and package qualification. The
[receipt](../coverage/evidence/M02c-authoritative-replication.json) records exact
inputs and results. Full **REQ-M02 remains NOT_RUN** until M02d
spatial interest/privacy/budgets and M02e integrated adversity complete.

## Delivered behavior

[ADR 0013](../architecture/0013-authoritative-replication.md) and the
[replication contract](../network-replication.md) define a CPU C++ publisher and
owned presentation cache. They use the existing Region Scene owner, authenticated
session authority and private TLS transport. No extra dependency or production
listener is introduced. Humans and terminal agents use the same APIs and
noninteractive JSON fixtures; authoring adapters retain their later owners.

- Trusted selection explicitly publishes at most 32 live entities per stream.
  Only the validated scene-local affine world pose is public. Name, hierarchy,
  durable identities, Scene handles/private revisions and unpublished entities
  never enter the state schema. Private-only changes produce no delta.
- Stream-local IDs never wrap/reuse. Baselines and sorted spawn/change/despawn
  deltas use a separate monotonic revision. One acknowledged state and one
  immutable pending update bound history. A full 32-to-32 replacement uses 64
  records in 3742 bytes, below the existing 4096-byte transport payload limit.
- ACKs mean successful application. Invalid/stale/future ACKs cannot consume the
  session sequence or renew its lease. Duplicate latest ACKs are idempotent with
  a fresh exact session sequence. Timeout requires resync or teardown; bounded
  resync creates a newer full baseline while retaining issued identity fences.
- Reconnect reclaims the old account stream only after preparing a valid new
  session/stream baseline. Old session IDs and controls cannot affect its
  replacement. Publication revalidates the actual live mTLS peer and readiness,
  without extending session lifetime merely by sending state.
- Client updates validate completely before publication. Missing baselines
  request recovery, exact latest duplicates are idempotent, and stale/conflicting
  traffic rejects. Cache loss retains revision and lifecycle fences; only a newer
  full baseline restores values. The cache is not another authoritative Scene.
- Replication version 1 encodes explicit big-endian binary64 coefficients,
  strict UUIDs, masks, lengths and reserved fields. Outer kinds 7/8 preserve
  client-control/server-state directions. Initial hello negotiation stays strict;
  diagnostic mode cannot carry replication traffic.

The API lives in `engine/core/include/shark/net/replication.hpp`; implementation
is split between the server/cache and wire translation units. The existing
session service gains a trusted `current` accessor with no lease/sequence side
effect beyond normal expiry maintenance. CMake registers the new source and
fixture targets; public headers expose no EnTT, TLS or socket implementation.

## Behavior verification

The new families are `shark.core.net-replication-wire` (20 s),
`shark.core.net-replication` (30 s) and `shark.core.net-replication-process` (90 s).
The first two use seed 1337 and `cpu;unit;server`; the process fixture uses
`cpu;integration;server`. Five codec cases exercise independent golden bytes,
every truncation, unknown fields, nonfinite/degenerate transforms, canonical zero
and exact maximum sizes. Seventeen service cases use real mTLS and controlled
clocks for ownership, bounded history/rates, timeout, rollback, generation reuse,
private changes, full replacement, reconnect and cache recovery. The mTLS suite
adds actual bidirectional delivery and direction/mode rejection.

The process fixture runs six scenarios in 17 child processes: convergence,
duplicate state, missing baseline, ACK rejection, reconnect and forged client
state. Five scenarios use one server plus separate A/B clients; forged-state
uses one server plus A. Both clients independently check public poses through
`{10,20}->{11,20,30}->{22,30}`, including spawn/despawn and stable replica IDs.
An unpublished parent contributes its transform but no hierarchy metadata.
Application ACKs gate source changes; payload canaries check private content.
Reconnect adds an actual third connection inside client A while B continues.
The harness checks final cleanup, exact public payload counts and actual socket
bytes, without logging credentials, session IDs or raw payloads. These semantic
faults do not claim encrypted TCP packet-loss qualification.

## Qualification

| Gate | Result |
| --- | --- |
| Windows MSVC Debug / Release / ASan | PASS, 72 CTests each |
| WSL GCC CPU client Debug / Clang client ASan+UBSan | PASS, 65 each |
| WSL GCC Server Debug / Clang Server ASan+UBSan | PASS, 50 each |
| Windows Runtime-only / Server-only build, lifecycle and install | PASS |
| WSL Release Server source, link, ELF, licenses and package closure | PASS |
| SDK-free WSL installed Server and runtime inventory | PASS, 11 files and unchanged 92-package inventory |
| Fresh Windows Runtime package and isolated launch | PASS, including all 10 missing-notice rejections |
| Public/private compile-input audit | PASS, 11 checks |
| Documented public C++ example | PASS, warnings-as-errors compile and compiler-reported public dependency closure |
| Actual sanitizer compile inputs | PASS, 116 private TLS C units in Windows ASan and both WSL Clang profiles |

All seven full profiles pass six replication scenarios in 17 child processes,
alongside the earlier transport and session fixtures. The independent replication
wire suite has five cases; the service suite has 17 named cases and
22 JUnit entries because asserting Catch2 sections produce extra entries.
The mTLS suite now has five cases. Loopback poll-loop assertion totals may vary
with scheduling; scenario and named-case inventories are fixed.

Windows and WSL each record the same 269 source/build/test/fixture inputs.
Every final hash matches; no source changes occurred during the final runs.
All 17 public PEM fixtures and the three pinned dependency archives remain
unchanged. Source/package closure preserves private static TLS and EnTT, central
public GLM, all eight TLS notices and the exact 11-file installed Server payload.
No test credential or probe executable enters either package.

GCC 13 initially rejected references returned by a variant visitor under
`-Wdangling-reference`. Copying the small envelope and stream identity values
removed the lifetime ambiguity without suppressing warnings. The failed WSL log
and interrupted Windows attempt remain separately recorded; final qualification
was restarted against the corrected frozen sources. These attempts are not
counted as passing evidence.

The first Windows Runtime package publication returned `WinError 5` during the
staging-directory rename. Cleanup left no published output. A fresh retry passed
publication, isolated launches and all ten missing-notice rejections without
changing source or host settings. The initial failure receipt is retained separately.

Formatting, contract/repository negative checks, whitespace, ignore and empty-index
checks are recorded in the receipt after final documentation verification.
No GPU behavior changed in this CPU-only increment.

Reproduce focused behavior after a native build:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R ^shark.core.net- --output-on-failure
```

Use existing WSL CPU/headless presets for Linux evidence. Retained local helper
entry points live under ignored `out/M02c-work`; no build requires these files
or the ignored local roadmap. Ordinary registered tests and dependency/archive
instructions remain in [building](../building.md).

## Limits and next checkpoint

This checkpoint proves bounded replication of explicitly selected scene-local
poses. Spatial relevance, measured byte/work budgets and head-of-line costs
remain **M02d**, followed by M02e integrated adversity. Global origin/region
transfer remains M05, durable storage M03 and combat/prediction M04. FeatureWorld
and MMOWorld connect these services in 10/M09; no placeholder authoring commands
or Lua bindings are added. Installed bootstraps have no public player listener
or test-auth defaults. Windows tools remain installed; isolated WSL userspaces
share this PC/kernel. No WAN, additional hardware or MMO capacity is claimed.

Next: **M02d - Spatial interest, privacy and connection budgets**.
