# Testing policy

Effective 2026-10-27, by user direction. All human contributors and AI agents
must use this policy to select and report verification for future work.

## Authority and scope

This policy supersedes older test-frequency, profile-matrix and evidence-process
instructions in AGENTS.md, skills, roadmaps (including the ignored local roadmap),
build/CI guides, architecture records, qualification contracts and handoffs.
Older lists of full suites,
public-example compiles, sanitizer-object audits, source freezes and package runs
describe their original qualification; they are not recurring subchunk gates.
This user-approved scheduling change needs no further approval to apply.

Behavioral acceptance criteria remain binding. Preserve existing tests, fixtures,
independent expected results, workloads, seeds, timeouts, dependency boundaries
and historical evidence. Preserving a test does not require executing it in every
configuration on every change. This policy changes when and how broadly checks
run, not what a selected test must prove. Current-machine scope and the user's
ownership of staging, commits and pushing remain unchanged. Hosted CI and remote
branch protection remain unchanged and do not become local prerequisites.

## Desktop-interaction pause, 2026-10-29

The user explicitly paused desktop/UI testing while using this PC for office
work, until further notice, and authorized continued coding. Do not use desktop
automation, move the mouse, send keyboard input, change focus, or launch/control
application windows while this pause is active. This includes automated windowed
GPU fixtures, even when no manual interaction is planned. Do not infer permission
from the time, date, elapsed duration or a later ordinary coding request. Only
the user's explicit instruction to resume desktop testing ends the pause.

Headless builds, CPU tests and other noninteractive checks may continue when they
do not open or interact with desktop windows. Required desktop checks remain
NOT_RUN/deferred and are nonblocking for continued coding under this explicit
user amendment. Record their exact remaining scope in the
[backlog](backlog.md#desktop-ui-deferral) and extend it as later checkpoints add
UI acceptance. Do not claim those checks passed or close full REQ-16 without
their required evidence. Completed native GPU results remain valid for their
tested inputs; the pause neither invalidates nor retroactively reruns them.

## Select checks before running them

State a short selection and rationale in the work update or existing task plan:
what changed, which consumers can be affected, and which checks will cover them.
Use the smallest selection that tests those effects. Do not create a separate
planning artifact or ask the user to approve ordinary test selection.

| Work being completed | Default verification |
| --- | --- |
| Documentation, policy or skill text only | Relevant existing link/manifest/repository checks and diff review. No engine configure, build, CTest, sanitizer or package runs. Compile examples only if their code changed. |
| Implementation in progress | New behavior and directly affected regression tests in the primary build. Add tests for meaningful behavior and failures, not trivial implementation details or wording. |
| A code subchunk | One primary Debug regression pass for the affected subsystem and consumers; affected tests in an applicable sanitizer configuration; an affected WSL build/smoke for portable code; new/changed public examples. This is a selected regression, not a mandatory full CPU inventory. |
| A full assignment/integrated checkpoint | Complete affected subsystem coverage in the primary Debug profile, the assignment's connected acceptance cases, relevant secondary-platform/Release/sanitizer selections, and affected standalone/package checks. A configured build's entire CTest inventory is not the default closure gate. |

Windows Debug is the normal primary build. WSL Server Debug is primary for
Server-only work; Windows-only work does not invent a WSL behavior gate.
Choose one relevant sanitizer profile by default. Use additional profiles when
compiler, platform, allocator, exception or optimization differences matter.
Run portable affected behavior on WSL; do not repeat all Tools integration tests
just to establish that a second configuration builds. A suitable existing run
on the final relevant inputs satisfies the check; do not repeat it at closure.

**Whole-project cleanup amendment, 2026-10-27:** "full applicable" means the
affected subsystem and its consumers, not every test available in a build.
The combined client presets also enable Server and include expensive dependency,
package and test-tool checks. Their availability does not make them applicable.
For example, Tools-only import qualification does not rerun unchanged network,
durable storage, graphics tooling or their dependency configuration matrices.
Use test names to select the owning subsystem (for example
`-R '^shark\.tools\.'` for the complete Tools behavior inventory), narrowing
further during implementation. Inspect the actual inventory before choosing the
selection; the broad `cpu`, `tooling` and `boundary` labels span several owners.
Add a dependency/configuration check when its inputs or boundary changed, not
merely because it appears in a preset. Bare CTest and `tools/ci.py` remain useful
explicit whole-project/hosted-CI workflows; do not use them as routine local
subchunk or single-subsystem closure commands. Hosted CI is unchanged.

Full assignment acceptance still requires all of its behavioral gates. Earlier
unchanged results may support unaffected areas when their applicability is
explained; they remain historical, never freshly executed results. A subchunk
pass does not close its whole assignment.

## Triggers for broader checks

Expand before assignment closure when the actual change creates a reason:

- Shared Core interfaces, ownership, scheduling or serialization: run affected
  client/Tools/Server consumers and integration tests. Widespread changes can
  justify full dependent Debug/sanitizer suites.
- Persistence, transactions, retries, concurrency or network behavior: run the
  relevant crash/recovery, lifetime, ordering and failure cases now. These do not
  wait for assignment closure when their implementation is being changed.
- Compiler flags, toolchains, allocators or instrumentation: check affected
  configurations and actual instrumentation where necessary. Inspect sanitizer
  objects for new or changed instrumentation-sensitive targets/sources or a
  concrete coverage doubt, not every unchanged object on every subchunk.
  For example, changes to the parser's MSVC exception/allocation boundary retain
  its Debug/Release/sanitizer failure sweeps; unrelated material fields do not
  require repeating that compiler-mode proof.
- Dependencies or CMake target graphs: verify the affected configurations and
  protected dependency boundaries. A Tools-only addition to the global lock
  does not by itself require full Server behavior suites, package redeployment
  or unrelated graphics artifact regeneration. Verify that protected inputs and
  closures remain unaffected; expand if they changed or that cannot be established.
- Install rules, runtime linkage, licenses, exported/cooked formats or package
  contents: run the affected package/relocation/isolated-launch checks. An explicit
  packaging deliverable requires its deployment checks; ordinary CPU edits do not.
- GPU, shader, window or presentation behavior: use the relevant graphics tests
  and available local GPUs. Pure CPU work does not trigger unrelated GPU runs.
- A failure or unresolved cross-system concern: investigate and expand only far
  enough to resolve it, explaining the connection to the change.

For example, 12d changes the import/cache/job path: its relevant stale-completion,
cancellation, dependency identity and last-valid-output tests belong in 12d.
The unrelated durable Server matrix is not automatically part of that selection.
12e performs the broader glTF integrated qualification.

## Evidence and stopping rule

Use existing CTest/CI tools, canonical build directories and ordinary logs/JUnit.
An existing handoff or a short test summary should record:

- Base revision and relevant worktree changes, selected commands/profiles,
  outcomes and log paths; include duration when available.
- What was not run and why, with any reused result's revision/scope and a brief
  explanation of why its relevant inputs are unchanged.
- Any failure, its resolution or remaining impact, and whether the result covers
  a selection or a full inventory. Repeated profile executions are not unique tests.

Do not create per-subchunk verification frameworks, bespoke source-freeze
ledgers, chains of hashed helper scripts, or exhaustive receipt audits by default.
Use existing required machine-readable report formats where consumed by tools;
add infrastructure only for an actual missing capability. Artifact hashes,
dependency locks and integrity checks required by the product remain intact.
Extra evidence mechanisms need a concrete reproduction, artifact or release
requirement. Preserve historical receipts without editing or regenerating them.

Keep one complete primary report and compact named outcomes for rejection
controls. Temporary copies of that report mutated to test its validator belong
in managed scratch; delete them after expected rejection, and retain unexpected
failures for diagnosis. Do not keep a full multi-megabyte copy per successful
negative control. Avoid repeating checkpoint histories in agent instructions and
skills; link the authoritative contract and handoff instead.

After the selected checks pass, stop testing and finish the deliverable. Repeat
only after relevant changes, a failure, or an unresolved concern. A documentation
edit after successful code checks requires documentation validation, not a fresh
engine matrix. Do not use a time budget to conceal failures, weaken acceptance,
remove coverage, or turn NOT_RUN into PASS. Investigate failures that affect the
selected behavior; report unrelated findings separately without silently adding
new release gates to the current subchunk.
