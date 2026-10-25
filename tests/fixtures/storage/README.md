# Independent durable-store fixtures

These source-authored SQL fixtures create real local SQLite files for M03e
migration and isolated restore tests. They are not generated from production
DDL, `Store::create`, or migration output. Each CREATE statement deliberately
retains the exact SQL spelling accepted by its historical store owner.

| Fixture | Reviewed source provenance | Durable content |
| --- | --- | --- |
| `schema-1.sql` | `d2c521ebb66f023bb5e1ff7d9c4936a2300518b3`, `server/storage/src/store.cpp` | The original metadata-only schema. |
| `schema-2.sql` | `64ffe0b8b2a00d4bbf75ef54c0562148b0ac69e1`, same path | Three accounts, four certificate enrollments and three owned character checkpoints. |
| `schema-3.sql` | `9dd0a925981c844f751abe8144a1ff204da404c8`, same path | The same character state plus two economic records and eight retained outcomes after nine consumed operations. |

All fixtures use distinct fixed UUIDv4 values: StoreId suffix 700, WorldId 900,
accounts 1/2/3, characters 101/102/103 and world entities 201/202/203 (decimal
suffixes rendered in hexadecimal). Accounts 1/2 are active; account 3 is revoked.
Roster revisions are 6/4/2; checkpoint revisions are 7/11/3. Names and positions
include nontrivial cell boundaries and fractional offsets. Account 1's synthetic
certificate ending in byte 2 is revoked; account 3's synthetic certificate ending
in byte 4 remains enrolled. The other enrollments contain SHA-256 fingerprints
of the existing `tests/fixtures/network/session/client-a.pem` and `client-b.pem`,
respectively, permitting actual mTLS tests after migration and restore.

Schema 3 models original balances 100/20 and 100/20. Operation 1 transferred 7/2
and has been pruned. Operations 2 through 8 record stale-revision rejection with
revision-2 snapshots. Operation 9 transferred 3/1 and remains replayable. The
final economic records are revision 3 with 90/17 and 110/23; the high-water mark
is 9. Restoring this fixture must preserve expired operation 1 and the exact
retained rejection/commit payloads. It must not grant resources, reset revisions,
or lower the operation high-water mark.

The fixtures are repository-owned test data with no external content license.
Generated database files, journals and evidence remain under ignored build/out
directories. These fixtures do not establish safe online rollback of a store
that has acknowledged changes after its backup snapshot.
