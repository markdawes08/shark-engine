# 09d: Safe retries and lost replies

Date: 2026-10-25. **09d is PASS under the current-machine scope.**
Assignment: the fourth of the [five authoring checkpoints](../roadmap/authoring-plan.md).
Base: `64535ae759e666a0aa8fd59a40a808fb81023f3d` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.
Final [qualification receipt](../coverage/evidence/09d-retained-retries.json): **PASS**.
Full REQ-09 remains **NOT_RUN**. **Next: 09e, Integrated qualification.**

## Behavior and contracts

[ADR 0023](../architecture/0023-retained-authoring-retries.md) and the
[retry contract](../authoring-retries.md) extend the same AuthoredScene owner.
`mutation_status` discovers the owner scope, current revision, next sequence,
retained range, counts, charges and limits. `mutate` accepts a scoped mutation ID,
required revision and typed edit, batch, undo, redo or clear-history operation.
`mutation_outcome` queries one owned immutable result. Check the outer Result,
the reply's outcome, and the original outcome's result: recording a rejected
operation does not mean its edit succeeded.

One fresh UUIDv4 owner scope and consecutive nonzero uint64 sequences fence
admission. The high-water mark never rewinds or wraps. Every admitted decision,
including noops and terminal validation/execution failures, is retained. Exact
matching includes expected revision, operation type, ordered commands and all
input fields, including raw double bits. Batch correlation is excluded; retained
errors keep their original correlation context. Identical retries return the
original immutable result and original observed revision. The enclosing reply
reports the current revision, even after unrelated edits or activation.

Defaults are 32 entries and 8 MiB; hard limits are 64 entries and 16 MiB. Payload
charge is bounded to 512 KiB, each Name input to 4096 bytes, and batches to 256
commands. Retained charges include owned capacities and a conservative fixed
outcome reservation, not allocator overhead or process RSS. FIFO pressure evicts
whole entries without forgetting the high-water mark. An entry that cannot fit
alone rejects before dispatch without eviction or sequence consumption.

The owned request, outcome storage and ledger slot exist before dispatch.
Existing services prepare effects before publication. Retention after successful
publication uses only nonthrowing ownership moves, bounded eviction and pointer
copies. Dispatch diagnostic failures are retained too. Caller-owned immutable
outcomes may outlive eviction or destruction; callers own those references.

Save, activation/reload and history clear preserve this owner's ledger. Activation
still clears history and replaces Scene incarnation. Every new owner, including
one reopened in the same process, has a fresh scope. Evicted or foreign IDs report
unknown outcome without execution, even with refreshed revisions or oversized
payloads. Inspect current state and reconcile before explicitly creating another
intent. There is no durable retry journal, time-based expiry or cache-reset API.
File publication and M03 economic durability retain their separate contracts.
Ordinary execute/undo/redo/clear calls remain nonretained and share Scene/history.

## Qualification

Five full CPU profiles pass with no failures or skips: Windows Debug, Release and
ASan each run 100 CTests; WSL GCC Debug and Clang ASan/UBSan each run 93. This is
**486 full-suite CTest executions**, plus **20 standalone Tools checks** (10 per
OS). All earlier test names, labels, seeds, timeouts, persistence cases and the
4096-entity/4095-delete workload remain. Core/Server implementation is unchanged;
the two full Server suites retain historical 09c evidence until 09e, as the
checkpoint plan permits. Both protected builds and packages are refreshed here.

The new `shark.tools.authoring-retries` family runs 25 cases with a 60-second
limit and seed 1337. It covers original successes/noops/errors, exact matching,
retention pressure, permanent expiry, counter exhaustion, owner/thread/visit/
pending fences, save/activation and allocation failures. All six mutation paths
use the 512-allocation sweep ceiling on Linux. Clear has two real allocations;
other operations require at least five observed faults. Windows uses six private
phase faults, including an admitted dispatch failure. These are controlled
single-fault checks, not a guarantee of recovery from repeated system OOM.

`shark.tools.authoring-retries-process` uses a 90-second limit and seed 1337.
Three real children drop a successful reply, save source, reject the old ID after
an intervening restart edit, reconcile explicitly, replay under the new scope,
save and reopen again. Parent checks persisted source bytes and strict receipts
independently; nine negative receipt controls run on each execution. This proves
process restart behavior, not crash-durable retry retention.

Seven public examples compile on MSVC, GCC and Clang: **21 compiles**, with
compiler-reported includes excluding private dependencies/headers. Actual ASan
symbols are audited in 13 native objects and ASan/UBSan symbols in 14 WSL objects,
including the new probe. Protected Runtime/Server builds and dependency closures,
relocated Runtime deployment, required licenses and an isolated SDK-free WSL
Server package pass. The latter retains 92 runtime packages and 12 installed files.

The source freeze retains **359 source/contract inputs and 16 helpers**. Its SHA-256
is `7c781d3c2c175b6b4da115f9948363c4f7eeb7e793d726b9b87fb45bf6c134f0`. Every receipt artifact is hashed, including the additional corrected metadata
verifier. The original frozen verifier is preserved. Exact commands, toolchains,
inventories, process receipts, object audits and limits are in the linked receipt
and `out/09d-work/`.

## Preparatory failures and reproduction

The first new process supervisor looked up the wrong source JSON field; it now
checks `components.shark.name.value`. The expanded Linux sweep exposed a new test
expectation that clear would allocate five times; it correctly performs only two.
A WSL boundary check also timed out at 10 seconds during concurrent native work.
Corrected WSL preflight passes both profiles without concurrent native builds,
and all final suites pass unchanged limits. All preparatory attempts are retained
and excluded from the final gate. No dependency behavior, host settings, previous
workloads, seeds or acceptance limits changed.

The first final aggregator expected seven native audit profiles because a public-
example count edit also touched that unrelated assertion. The corrected verifier
requires the six exact planned profiles and 13 distinct objects, while separately
requiring seven public examples. Seven generic and five native-audit negative
controls pass. The frozen inputs, original verifier and completed run records are
preserved; this metadata correction does not change any executed acceptance gate.
See `out/09d-work/aggregation-correction.json` and the receipt's explicit verifier
hash and control references.

Run from the repository root with the native wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09d-work/verify-windows.py final
.tools/venv/Scripts/python.exe out/09d-work/launch-wsl.py final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09d-work/native-audit.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09d-work/verify-runtime-package.py
```

Evidence destinations are intentionally create-only; preserve completed attempts
before preparing a fresh run. The ignored helpers are local recipes, not checkout
or CI prerequisites. Versioned focused tests can also run through CTest with
`-R ^shark.tools.authoring-retries`. No GPU acceptance is introduced by 09d.

09e connects commands, preview/batches, history, retries and file/external-change
conflicts through one qualified lineage. Only 09e closes full REQ-09. FeatureWorld
remains 10; terminal/project ownership and editor integration remain 15/16. No
terminal/editor adapter or player protocol is introduced here.
