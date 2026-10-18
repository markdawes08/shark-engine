# M02a: Local TLS transport and wire foundation

Date: 2026-10-18. Starting revision:
`3c5767a95e9944edc3d09ce7d408805899ff8025` on `main`, initially clean.
Changes remain uncommitted; no result revision exists. The user owns staging,
commits and pushes.

**M02a is PASS under the current-machine scope.** All 404 CPU CTests pass
without failures or skips, together with protected build/source/link/license and
isolated package checks. The [qualification receipt](../coverage/evidence/M02a-transport-wire.json)
records the observed results and exact inputs. Full **REQ-M02 remains NOT_RUN**:
player sessions, replication, interest/privacy budgets and integrated adversity
retain M02b-M02e.

## Delivered contracts

EngineCore adds three shared CPU C++ interfaces:

- [wire.hpp](../../engine/core/include/shark/net/wire.hpp): explicit independent
  wire version 1, a 20-byte big-endian header and at most 4096 payload bytes.
  Diagnostic hello/ack/probe/reply kinds have exact validation. Unknown versions,
  flags or kinds, oversized claims, zero/replayed/skipped sequences and truncated
  input reject. The bounded incremental decoder handles fragmentation and
  coalescing; sequence exhaustion never wraps.
- [transport.hpp](../../engine/core/include/shark/net/transport.hpp): owned,
  nonblocking IPv4 loopback TLS listener/connections, one creating thread per
  object, explicit certificate/trust inputs and caller-driven monotonic polling.
  Independent input/output queues, admission limits, handshake/idle/write
  deadlines and bounded poll steps reuse the existing Core error/time contracts.
  Accepted connections retain their TLS configuration after listener destruction.
- [fault_link.hpp](../../engine/core/include/shark/net/fault_link.hpp): a seeded,
  virtual-time semantic message/datagram model for delay, jitter, loss,
  duplication, reordering and partitions. Packet/count/byte/drain limits are
  explicit; backpressure and invalid input have tested state/RNG behavior.

[ADR 0011](../architecture/0011-network-foundation.md) records the selection and
threat assessment. The [network contract](../network-transport.md) owns the exact
wire layout, resource limits, error/lifetime rules and fault-model distinction.
TLS is restricted to 1.3, AES-128-GCM/SHA-256, P-256 key exchange and
ECDSA/P-256/SHA-256 handshake signatures. Clients require the supplied CA, expected
DNS name and valid certificate chain/time; there is no implicit system trust or
insecure verification switch. Native socket and TLS/PSA types stay private.

M02a authenticates the server endpoint, not a player. Server/Runtime gain no
network CLI flags or gameplay listener. The test-only `NetworkProbe` drives the
real process fixtures. Public test certificates/keys are original Shark fixtures
under [tests/fixtures/network](../../tests/fixtures/network/README.md), never a
default runtime identity or packaged authentication bypass. No authoring protocol,
administrative capability, player intent, scene replication or persistence is
introduced.

## Dependency and protected graph

[dependencies.lock.json](../../dependencies.lock.json) pins the official full
Mbed TLS **4.1.1 LTS** release archive:

| Input | Exact identity |
| --- | --- |
| Mbed TLS commit | `0a8fda272a5a0abef3b47c91bed37185d5a726b1` |
| Release archive SHA-256 | `3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c` |
| Bundled framework commit | `dde0c4a0e448a0552f18817dcea633bb851fd288` |
| Bundled TF-PSA-Crypto commit | `a0632be94d883daa0295ac1eabf70359ad94f91b` |

The official release includes generated sources and nested inputs absent from
GitHub's automatic source archives. Release notes and bundled crypto version
metadata disagree; the exact archive and commits identify the reviewed input.
Shark selects the Apache-2.0 license option and retains eight complete notices.
No existing EnTT, GLM or graphics pin changes.

`NetworkDependencies.cmake` integrates three private static libraries and seven
reviewed crypto object targets. C compilation is now required in every Core
configuration. C/C++ compilers use matching families; sanitizer presets also
instrument these private C targets. Windows socket/entropy libraries are
explicitly classified OS imports. Public GLM through `SharkMath` remains the
sole math exception; neither EnTT nor TLS headers propagate to consumers.

The fixed archive helper validates all **3,927 regular files**, totaling
60,515,866 bytes, and omits only **147 pinned symlinks** in unused PQC test/example
trees. No source patch, arbitrary source/configuration override, installed TLS
fallback, hidden transitive download or upstream test/program build is accepted.
Every configure verifies populated TLS bytes; source drift fails rather than
being silently repaired. Disconnected configurations require the verified
`SHARK_MBEDTLS_ARCHIVE` alongside EnTT/GLM inputs or their prior locked caches.

Headless closure checks cover the fixed C/C++ target/source inventory, actual
compiler-discovered headers, the three TLS archives plus EngineCore, reviewed
object artifacts tied to compiler outputs, ELF dependencies and all installed
notices. Runtime and Server packages require the eight `licenses/MbedTLS/`
notices in addition to EnTT/GLM and applicable existing client notices. No shared
TLS library or public test credential belongs in either package.

## Tests and findings

The four new CTest families are registered in ordinary and headless builds:

| Family | Coverage | Timeout |
| --- | --- | --- |
| `shark.core.net-wire` | Independent golden bytes, fragmented/coalesced parsing, malformed fields, truncation and sequence bounds | 20 s |
| `shark.core.net-fault` | Fixed-seed schedules, loss/duplicate/reorder/partition behavior, queue/byte limits, owner checks and atomic rejection | 20 s |
| `shark.core.net-transport` | Configuration/role validation, owned lifetimes, thread rejection, queue pressure and monotonic deadlines | 30 s |
| `shark.core.net-process` | Separate local engine server/two-client processes, independent TLS peers, trust failures and malformed wire input | 60 s |

The three C++ families use seed 1337. Process evidence records fixture hashes,
commands, exits, machine-readable results and watchdog cleanup. Its independent
Python TLS peers keep ordinary TLS 1.3 session tickets enabled. They test clean
close-notify after a complete reply, partial-frame EOF, wrong wire version,
replayed sequence, unknown kind and oversized advertised payload. Other real
process scenarios reject wrong CA, wrong DNS name, expired certificate, stalled
handshake, abrupt EOF and refused connection. Virtual-time message faults remain
separate from these real reliable-stream cases.

Prequalification review fixed two observable interoperability/lifetime issues:

- A complete validated reply must remain drainable when authenticated
  close-notify arrives during the same poll. Clean peer closure now preserves
  queued owned frames; explicit local abort and failed streams discard them.
  Partial-frame clean EOF still fails at the wire boundary.
- A TLS 1.3 peer may send a new-session-ticket notification even when Shark
  never resumes a session. Polling now consumes that notification within its
  existing step budget, without exporting/reusing the ticket. The final
  independent-peer fixture retains tickets; disabling them was an investigation,
  not the final interoperability condition.

The refused-connection fixture permits the bounded OS refusal response or the
configured transport deadline. Windows can report refusal after approximately
two seconds; the fixture reserves a non-listening socket to avoid port reuse.
It does not convert a successful connection into an expected failure.

Focused dependency preflights pass: 34 adversarial closure cases, eight scene
dependency cases and eight graphics dependency cases. The latter two measured
approximately 134 and 131 seconds respectively with fresh TLS populations.
Their tooling suite deadlines are 240 seconds, with 120-second configure
deadlines; transport acceptance limits are unchanged. Native boundary configure
and an actual WSL Release Server source/link/ELF/notice preflight also pass.
These preflights do not substitute for the final matrix below.

## Qualification and reproduction

The completed matrix contains **404 passing CTests, zero failures and zero skips**.
Each profile runs the real process fixture with 20 engine child processes and six
independent Python/OpenSSL TLS peers. Native family reports record 12 wire cases,
11 fault-model cases and 10 transport cases; assertion counts remain in the receipt.

| Gate | Result |
| --- | --- |
| Windows MSVC Debug, Release, ASan full CTest profiles | PASS, 66 each (198 total) |
| Windows Runtime-only and Server-only protected builds, Server lifecycle/install | PASS |
| WSL GCC client Debug and Clang client ASan+UBSan | PASS, 59 each (118 total) |
| WSL GCC Server Debug and Clang Server ASan+UBSan | PASS, 44 each (88 total) |
| WSL Release Server source/link/license/package closure | PASS |
| Retained SDK-free Ubuntu installed-Server run and inventory audit | PASS; exact 11-file package, unchanged 92-package inventory |
| Fresh Windows Runtime package, isolated launch and all 10 missing-notice rejection cases | PASS |
| Public/private compile-input audit | PASS, all 11 checks across three Windows profiles |
| Formatting, contracts and repository checks | PASS; 134 formatted sources, 29 contract and three repository negative cases |

Both platform qualification drivers verified all 243 recorded source inputs stayed
unchanged during their runs. Every one of the 116 private TLS C translation units
is sanitizer-instrumented in Windows ASan and both WSL Clang profiles. The later
CI-only amendment below has separate red/green evidence; the compiled engine,
dependency and test inputs remain exactly those qualified by the matrix.

Exact local drivers and their expanded commands are retained under ignored
`out/M02a-work/`. Run from the repository root in the correct compiler environment:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M02a-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M02a-work/wsl-run.sh
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M02a-work/verify-runtime-package.py')
```

These drivers require fresh evidence/package output paths. The Windows driver
configures/builds `windows-debug`, `windows-release`, `windows-asan`, `runtime-only`
and `server-only`, explicitly passes all three archives, runs full CTest with
JUnit, records the new C++ families separately and installs the Server component.
The WSL wrapper uses the retained pinned Ubuntu userspace with the checkout
mounted read-only; `wsl-verify.py` selects GCC 13 or Clang 18 for both languages,
keeps native window/graphics OFF, checks real source/ELF closure and records full
CPU/headless CTests. Its separate runtime root supplies the SDK-free installed
Server check. The Runtime package driver verifies file/DLL/notices, relocates to
a Unicode/space path outside the checkout, launches help/version with system-only
PATH and removes each required notice in separate rejection fixtures.

Durable preparation/build commands remain in [building](../building.md) and
[CI](../ci.md). Raw profile logs, JUnit and `network-process.json` records remain
under `out/M02a-work/windows-final/` and `out/M02a-work/wsl-qualification/`.
Dependency-review hashes are in
`out/M02a-work/dependency-review/summary.json`. The versioned
[receipt](../coverage/evidence/M02a-transport-wire.json) retains the matrix, process
results, source maps, package evidence and hashes of the detailed local records.

**Separately verified postqualification amendment:** `tools/ci.py` now forwards
`CC` alongside `CXX`, preserving explicitly selected versioned C/C++ compilers over
portable preset defaults. The qualification drivers already selected matching
compilers. After both platform audits completed, this two-line helper change was
checked through real `ci.main` command construction with execution/toolchain probes
mocked. The versioned CC/CXX case failed before the fix; both that case and the
unset-default case pass afterward. Run the retained check with:

```text
.tools/venv/Scripts/python.exe out/M02a-work/verify_ci_compiler_forwarding.py --output out/M02a-work/ci-compiler-forwarding-after.json
```

`out/M02a-work/ci-amendment.json` and the versioned receipt retain the exact diff,
old/final hashes and both outcomes. The full matrix was not rerun through this
helper; no compiled input changed. Only `tools/ci.py` differs between the qualified
and final source maps, and final contract/repository/format checks pass.

## Scope, coverage and next checkpoint

Qualification is limited to this Windows PC and its existing WSL2 Ubuntu CPU
environments under ADR 0010. No new GPU execution is required by this CPU-only
increment. Windows development tools remain installed; WSL shares the host kernel
and the retained runtime root is writable. No Mac, hosted-runner, independent
machine, cloud identity provider, public certificate, WAN or production deployment
qualification is claimed. No host/WSL settings are changed.

TCP head-of-line blocking remains a documented transport tradeoff. Snapshot
replacement may occur before reliable FIFO admission; already transmitted bytes
cannot be overtaken. Step/queue limits are not hard CPU-time or total RSS limits.
The two-client diagnostic fixture does not prove ADR 0007's LOCAL-MMO-1 workload,
authoritative tick budgets, player authorization, replication convergence,
interest privacy, persistence or seamless region transfer.

Coverage adds `core.network-foundation` under REQ-M02 with the executable
wire/fault/transport/process and dependency/package fixtures. Authoring command
and Lua APIs remain unchanged. FeatureWorld starts in 10 and MMOWorld acceptance
in M09; local TLS fixtures are the justified external coverage for this checkpoint.
Generated builds/evidence stay ignored under `build/` and `out/`; the local
roadmap and local credential material remain excluded, while the explicitly
public test fixtures remain source inputs.

M02a's gate and separate CI amendment are complete. The exact next checkpoint is
**M02b: authentication and owned sessions**, under the
[five-checkpoint M02 plan](../roadmap/network-plan.md). Durable account/character
storage remains M03. Do not mark full REQ-M02 PASS at this foundation checkpoint.

Suggested commit: `feat(net): add bounded TLS transport and wire foundation`
