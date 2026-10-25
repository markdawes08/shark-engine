# Integrated durable-state qualification

Owner: M03f / REQ-M03. This gate connects the existing Server-only
[store](durable-store.md), [owned characters](durable-characters.md),
[transactions](durable-transactions.md), [runtime recovery](durable-recovery.md)
and [maintenance](durable-maintenance.md) services. It changes neither schema 3
nor the player wire protocol. Qualification status is recorded separately.

## Connected scenario

One independently authored legacy store passes through migration, authenticated
runtime use, interrupted economic work, recovery and isolated restoration. Each
phase consumes the previous phase's actual database. A fresh process must inspect
and reconcile durable state; a collection of unrelated successful fixtures is
not evidence of continuity.

The fixture uses the existing schema-2 SQL input and its explicit identities and
revocations. It first creates and validates the exact pre-upgrade backup, then
migrates through the production maintenance service. Trusted one-time economic
initialization is the recorded source of resources; migration creates none.
Real mutually authenticated client processes use the existing bounded session
messages. The trusted server fixture maps those messages to fixed operations;
it adds no shipping storage protocol or player-controlled checkpoint write.

Runtime save and transfer use the same CharacterAuthority, dedicated Store
worker and actual Scene references. Two accounts contend using the same economic
revisions. The fixture checks conservation, one committed transfer, the competing
terminal stale-revision result and exact durable retries. A server crash after
COMMIT loses the acknowledgement; a fresh owner must reconcile the original
operation identity without applying the transfer again.

An observed native SQLite VFS failure exercises the actual database I/O path.
Admission faults, retained work is drained, and explicit recovery reopens the
same store. Fresh authenticated Login is required. Retained outcomes and durable
high-water marks distinguish retry, changed payload and permanently expired
operations. Persisted revocation must still deny access after restart.

After the live owner stops, a verified same-point backup is restored into a new
directory. A fresh owner opens the candidate and proves identity, checkpoints,
economic state, retained outcomes, expiry and revocation. No acknowledged write
may occur between that backup and the same-point restart. Older snapshots remain
isolated inspection evidence; they never authorize rollback or a second live
authority with the same StoreId.

The fixed identity set is Store 700 / World 900, accounts 1/2/3, characters
101/102/103 and world entities 201/202/203, encoded by the existing SQL fixture.
Account 3 and the existing certificate tombstone stay revoked throughout.
Account 1's checkpoint advances from revision 7 to 8; the other character
revisions remain 11 and 3. Both economic records start at revision 1 with
100 currency and 20 item units each.

| Account 1 operation | Request | Expected durable result |
| --- | --- | --- |
| 1 | Transfer 7 currency / 2 items at revision 1 | Commit; both economies become revision 2 |
| 2 | Transfer 3 / 1 at revision 2 | Commit before process exit; original-ID retry is a replay |
| 3 | Transfer 4 / 1 at revision 3 | Observed write failure leaves it uncommitted; fresh-owner original-ID retry commits revision 4 |
| 4 through 10 | Transfer 1 / 0 using stale revision 1 | Seven durable stale-revision outcomes; no resource change |
| 11 | Transfer 2 / 1 at revision 4 | Commit revision 5; final balances are 84 / 15 and 116 / 25 |

Account 2's competing operation 1 requests 5 currency / 1 item at revision 1
and retains its stale-revision outcome. Account 1's final high-water mark is 11,
with exactly outcomes 4 through 11 retained. Operations 1 and 3 remain expired
after restore; changed payload under retained ID 11 rejects. Account 2's later
durable revocation denies both its live session and restored Login. Total
currency and items remain 200 and 40 after trusted initialization.

## Evidence requirements

`shark.storage.integration-process` has labels `cpu;integration;server` and a
300-second CTest limit. Its seven ordered phases launch sixteen children and
establish nine actual mutually authenticated TLS connections:

| Phase | Child roles | Required connection |
| --- | --- | --- |
| `prepare` | One maintenance process | Independent schema 2, exact backup, migration and explicit initial resources |
| `runtime` | Server and two clients | Authenticated checkpoint save and competing economic revisions |
| `commit-crash` | Server and one client | Actual transfer COMMIT followed by lost acknowledgement |
| `outage` | Server and two clients | Fresh owner reconciles the committed request, observes I/O failure and explicitly recovers |
| `recover` | Server and two clients | Fresh admission, original-ID retry, retention expiry and durable revocation |
| `restore` | One maintenance process | Stopped-owner backup and isolated same-point candidate |
| `restored` | Server and two clients | Fresh admission, exact durable state, stale-session/revocation/expiry denials |

Each child has a 30-second limit, readiness has an 8-second limit and at most
three fixture children run simultaneously. The supervisor uses fixed schedules,
controlled monotonic observations and bounded readiness barriers. The existing
69-row recovery matrix supplies detailed precommit interruption evidence; the
connected lineage exercises the postcommit lost-acknowledgement path.

The process supervisor validates exact phases, child roles and exit outcomes,
observed fault events, immutable identities, expected revisions and complete
bounded state. Expected outcomes come from the declared fixture schedule rather
than trusting a child's PASS flag or record counts. The 38 negative controls must fail
through the actual supervisor's nonzero exit and machine-readable error path.
All child trees and waits are bounded. The fixed probe emits bounded state;
accepted child JSON is limited to 1 MiB and any stderr rejects. This validation
happens after the shared process runner captures output; it is not an OS pipe or
disk quota. Missing or malformed evidence cannot be relabeled as success.

Retain all earlier storage and network test families. Their detailed precommit,
postcommit, lost-reply, timeout, revocation, migration and native-I/O matrices
remain required alongside this connected scenario. Freeze implementation,
fixture, build, contract and qualification-helper inputs before the final matrix.
Run native Windows Debug/Release/ASan and WSL client/server Debug and ASan/UBSan,
standalone CPU Tools, protected Runtime/Server packages and all five published
storage C++ examples. Verify actual sanitizer objects and private dependency
closures, not only successful configuration flags.

## Scope and next owner

This is a bounded local durability workload on the current Windows PC and its
existing WSL environment. It does not qualify the full LOCAL-MMO-1 population,
20 Hz simulation latency, continuous region travel, production capacity or
physical power/disk/host loss. Character checkpoints and economic effects have
zero permitted authoritative rollback after acknowledgement. There is no backup
cadence or elapsed-time checkpoint-loss allowance.

Human and terminal contributors run the same CPU services and noninteractive
fixtures. No editor, GPU, cloud account or LLM SDK is needed. FeatureWorld and
MMOWorld retain owners 10 and M09; gameplay, region authority, operational
administration and terminal/editor adapters retain their assigned owners.
After the complete M03f gate passes, the next assignment is **09: Shared
authoring commands and undo/redo**.
