# M02b: Authentication and owned sessions

Date: 2026-10-18. Starting revision:
`702c02d8122fef0331a899b00a4f2eeded694e25` on `main`, initially clean.
Changes remain uncommitted; the user owns staging, commits and pushes.

**M02b is PASS under the current-machine scope.** All 425 CPU CTests pass
without failures/skips, alongside protected builds and isolated package checks.
The [receipt](../coverage/evidence/M02b-owned-sessions.json) records actual results
and exact inputs. Full **REQ-M02 remains NOT_RUN**: replication,
interest/privacy budgets and integrated adversity retain M02c-M02e.

## Delivered behavior

[ADR 0012](../architecture/0012-owned-sessions.md) extends the M02a TLS foundation
with certificate-based mutual authentication and explicit trusted enrollment.
The [session contract](../network-sessions.md) defines the shared CPU C++ API,
wire layouts, bounds and lifetime rules. Human developers and terminal agents
use the same service and noninteractive fixtures, without an editor or model SDK.

- The existing private Mbed TLS 4.1.1 library verifies client trust and proof of
  private-key possession. Shark requires explicit clientAuth EKU,
  digitalSignature usage and a non-CA leaf. Both sides verify certificate
  validity during the handshake; each authority request also rechecks the copied
  client-certificate validity interval. Peer DER fingerprints and validity
  intervals are owned copies; TLS/socket types remain private. Certificate-based
  ephemeral TLS 1.3 excludes PSK resumption and 0-RTT authorization.
- Trusted owner operations provision typed accounts, immutable character
  ownership and certificate enrollment. `SessionAuthority::apply` requires the
  actual ready server-side mTLS transport on every request; a copied fingerprint
  or peer record cannot substitute for it. Existing diagnostic connections
  cannot carry session requests or supply authenticated player identity.
- Login validates the credential/account/character relationship, issues an
  OS-entropy session UUID and increments a nonwrapping account-wide ownership
  epoch. One active session is allowed per account. A replacement must use a
  server-side connection created after the account's previous successful login
  connection and pass all validation before replacing the old session. Stale
  queued commands, old transports and revocation by the old session ID cannot
  affect the replacement.
- SetReady, Heartbeat and Logout validate connection, session, epoch, character
  and exact next sequence. Ready is session state and grants no gameplay,
  authoring or administrative capability. Authority failures map to a uniform
  four-byte session reply payload without foreign identity data or internal
  error details; the outer wire frame is separate.
- Monotonic lifetime/idle deadlines, global and credential/account login
  windows, command budgets and bounded registry/tombstone storage limit admission
  and retained state. Reconnect cannot reset credential/account rate windows or
  ownership fences. Revocation immediately removes affected live sessions.
  Failed requests cannot refresh leases, advance accepted sequences or replace
  still-valid ownership; advancing time may expire an existing lease. Connection
  owners can explicitly revoke their last session ID on disconnect without
  revoking a newer replacement. The authority retains no borrowed transport.

The outer wire keeps version 1 and adds session request/reply kinds 5/6. Its
independent session version 1 uses 36 bytes for Login, 53 for SetReady and 52 for
Heartbeat or Logout; accepted/rejected replies use 70/4 bytes. The codec checks
big-endian fields, UUID v4/variant bits, versions, exact lengths, reserved-zero
bytes, booleans, epoch/sequence and canonical reply state.
Existing diagnostic golden bytes remain unchanged.

No public Server/Runtime player-listener flags, credentials or authentication
bypasses are introduced. Public test keys stay under
[tests/fixtures/network/session](../../tests/fixtures/network/session/README.md).
An independent OpenSSL review verifies their key pairs, DER fingerprints, dates
and client-purpose expectations. Parent M02a fixture bytes are unchanged.

## Tests and findings

Three CTest families join the existing four network families:

| Family | Coverage | Timeout |
| --- | --- | --- |
| `shark.core.net-mtls` | Real TLS identity, strict client certificate profiles, private-key pairing, connection tokens, roles, threads and lifetimes | 30 s |
| `shark.core.net-session` | Exact wire golden bytes and malformed payloads; ownership, rates, expiry, revocation, reconnect and bounded service state | 30 s |
| `shark.core.net-sessions-process` | Real server and client processes, distinct enrolled credentials and adversarial session operations | 90 s |

The unit suites contain four mTLS and 16 named session cases, using seed 1337.
Catch2 emits 19 session JUnit entries because three revocation sections also
have separate entries.
Deterministic service clocks cover exact lease/rate boundaries. TLS poll-loop
assertion totals can vary with scheduling; case counts and meaningful inputs are
fixed. The independent session codec fixtures mutate field bytes directly and
exercise every truncation, additional bytes and incompatible versions.

Each full profile passed **19 session scenarios in 39 child processes**:
two separate identities; absent, wrong-CA, expired, wrong-EKU, missing-EKU, wrong-KU
and CA-leaf credentials; unregistered/revoked enrollment; wrong account and
character ownership; replay; malformed/versioned payloads; real expiry; live
revocation; rate limits; overlapping reconnect and cross-connection theft.
The process receipts retain counters and rejection operations without PEM contents,
private keys, session IDs or request/reply payloads. Credential-negative cases
must fail at the server's TLS handshake/client-profile boundary; a timeout,
connection failure or clean EOF cannot count as successful authentication
rejection. Every process has a watchdog and cleanup evidence.

An accepted reply with `ready=true` and sequence 0 rejects on both encode and
independent decode because readiness requires an accepted command. The outer-wire
unknown-kind fixture uses kind 7; kind 5 now denotes a valid session request.
All seven focused network families passed after rebuilding these changes. The
final matrix below uses those frozen sources.

## Qualification and reproduction

The completed matrix contains **425 passing CTests, zero failures and zero skips**:

| Gate | Result |
| --- | --- |
| Windows MSVC Debug and Release full CTest profiles | PASS, 69 each |
| Windows MSVC ASan full CTest profile | PASS, 69 |
| Windows Runtime-only/Server-only builds, lifecycle/install | PASS |
| WSL GCC CPU client Debug and Clang client ASan+UBSan | PASS, 62 each |
| WSL GCC Server Debug and Clang Server ASan+UBSan | PASS, 47 each |
| WSL Release Server source/link/ELF/license/package closure | PASS |
| SDK-free installed Server and runtime inventory audit | PASS; exact 11-file package, unchanged 92-package inventory |
| Fresh Windows Runtime package, isolated launch and all 10 missing-notice rejections | PASS |
| Private/public compile-input audit | PASS, 11 checks across three Windows profiles |
| TLS sanitizer compile-input audit | PASS, all 116 private C units in Windows ASan and both WSL Clang profiles |
| Formatting, contracts, repository and ignore/index checks | PASS; 140 C++ files, 29 contract and three repository negative cases |

The Windows source map contains 258 inputs; WSL adds four protected build/package
inputs for a combined 262. All overlapping hashes agree and final aggregation
verifies every input still matches. Both platform drivers report no changes.
Each profile retains the earlier 20-engine-process/six-independent-peer diagnostic
fixture as well as the 19-scenario/39-process session fixture. Native case counts
are 12 wire, 11 fault-model, 10 transport, four mTLS and 16 named session cases
(19 session JUnit entries).
No GPU behavior changes in this CPU-only increment.

Protected Runtime-only and Server-only builds retain the exact dependency graph.
Dependency pins, upstream source patches and license selections are unchanged.
The M02a archive review remains historical evidence; fresh build/closure audits verify current
Core sources and the same locked EnTT, GLM and 3,927-file TLS population.
The installed Server package contains exactly 11 files, including all eight
TLS notices. No test certificate, private key or shared TLS library is packaged.

Retained local drivers under ignored `out/M02b-work/` require fresh evidence and
package paths. Run from the checkout in the proper compiler environment:

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M02b-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M02b-work/wsl-run.sh
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M02b-work/verify-runtime-package.py')
```

The Windows driver selects all three pinned offline dependency archives, builds
three full profiles plus Runtime-only/Server-only, records full CTest/JUnit and
both process receipts, and installs Server. The WSL driver mounts the checkout
read-only in the retained pinned Ubuntu build userspace, selects matching GCC 13
or Clang 18 C/C++ compilers, keeps native window/graphics OFF and checks real
Release source/link/ELF/license closure. It runs the four CPU profiles and starts
a fresh package in the separate SDK-free runtime with an isolated network.

The Runtime package check relocates outside the checkout to a Unicode/space path,
uses system-only PATH for help/version, inspects DLL/notices and rejects each of
ten missing required notices. The public/private input audit inspects actual
compile databases. The sanitizer audit checks every private TLS C unit and the
session/transport/test units. Final checks cover formatting, contract/repository
negative cases, whitespace, ignore rules and the untouched Git index.

For routine verification after building, use:

```text
ctest --preset windows-debug -R "^shark.core.net-" --output-on-failure
```

The corresponding WSL CPU/headless presets exercise the same services. Durable
build commands are in [building](../building.md) and [CI](../ci.md). Machine-readable
`network-process.json` and `network-sessions.json` under each build's `tests/`
provide bounded outcomes without parsing console text.

## Scope and continuation

Qualification is restricted to this Windows PC and existing WSL under
[ADR 0010](../architecture/0010-current-machine-scope.md).
Windows development tools remain installed; the SDK-free WSL userspace shares the
host kernel and remains writable. No host settings, remote infrastructure,
external identity account or additional machine is required or changed.

Enrollment, immutable ownership and revocation are in-memory trusted operations.
Durable storage/recovery remains M03. Polling/expiry are caller-driven on one
owner thread; queue/capacity/step limits are not hard CPU-time or RSS guarantees.
Session acceptance makes no replicated-state, interest/privacy, WAN, active MMO
capacity or durable exactly-once claim. TCP head-of-line measurements and active
entity budgets remain M02d/M02e.

Coverage adds `core.owned-sessions` under REQ-M02. Existing public authoring
commands, Lua and component schemas are unchanged; their adapters retain their
assigned checkpoints. FeatureWorld/MMOWorld integration remains assigned to 10/M09.
Generated builds, local credential experiments and evidence remain under ignored
roots; the local roadmap is ignored. Source fixtures and documentation remain
versioned inputs.

The exact next checkpoint is **M02c: authoritative
replication and resynchronization**, following the
[five-checkpoint M02 plan](../roadmap/network-plan.md).

Suggested commit: `feat(net): add authenticated owned sessions`
