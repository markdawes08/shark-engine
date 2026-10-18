# Windows/Linux platform scope amendment

Decision date: 2026-10-17 (America/New_York). Base revision: `21239e7`
(`feat(graphics): add portable Windows probe packaging and soak evidence`).
Result revision: uncommitted working tree; staging, committing and pushing remain
with the user. Assignment 02 remains current.

The user explicitly removed all Mac work and compatibility requirements.
[ADR 0009](../architecture/0009-windows-linux-platform-scope.md) records that
decision and supersedes the earlier [02c deferral](02c-mac-deferred.md).
Checkpoint 02c is retired, not pending or qualified. Windows 11 x86-64 and
Ubuntu 24.04 x86-64 clients/editor remain the selected targets; the Linux
headless Server and human/terminal-agent authoring requirements are unchanged.

## Changes and existing-work assessment

Active architecture, requirements, execution plans, build/probe guides, README,
repository instructions and skills now use the Windows/Linux scope. The ignored
local roadmap is amended consistently. Assignment IDs and order, the requirement
schema and existing feature/API mappings are unchanged. No new engine, component,
command, Lua or FeatureWorld surface is introduced.

The CPU workflow removes six Mac jobs and their Xcode preparation. Matching Mac
CMake/CI presets and platform-tool preparation promises are removed. The remaining
six CPU configurations plus `quality`, `runtime-only` and `headless-server` form
nine checks. The offline CI validator checks that matrix and the retained presets.
Graphics configuration targets Windows/Linux; unavailable tool hosts return
NOT_RUN without attempting qualification work. Darwin-only dependency options
and the open Mac shader-tool TODO are removed from the lock; supported dependency
versions and Windows/Linux dependency options are unchanged.

The existing Core/Client/Server separation, Vulkan/NVRHI probe, Windows package
and terminal tooling do not need a production runtime rewrite for this scope
change. Generic portability branches, upstream options, broad wheel hashes and
OS metadata ignores may remain without a Mac support or maintenance commitment.
No graphics algorithm, shader, validation tolerance or lifecycle budget changes.

Historical handoffs and machine-readable CPU/GPU receipts retain their original
platform counts, input hashes and outcomes. Their Mac rows no longer impose a
current gate. The amended active matrix contains six Windows/Linux rows, all
still NOT_RUN; supplemental RTX 4070 Laptop and Raptor Lake results do not qualify
the named GTX 1660, RX 5500 XT or Tiger Lake floor. The prior Windows relocation
remains development-host evidence, not clean-machine qualification.

Removing lock metadata changes the complete dependency-lock fingerprint; the
native probe source fingerprint is unchanged. The old 02d package/receipts remain evidence for their
recorded inputs. Rebuild the probe, shader/validation receipts and portable
package before a new current-input GPU run; do not relabel the old package as
current. No GPU rerun is required to record this scope decision.

`.gitignore` was reviewed: the local roadmap, generated builds, tools, packages
and evidence under `out/` remain ignored; new decision/handoff sources remain
eligible for versioning. No additional ignore rule is needed.

## Verification and repository policy

Local host: Windows 11 x64 build 26200, MSVC 19.51.36248 / toolset 14.51.36231,
Windows SDK 10.0.28000.0, Python 3.13.5, CMake 3.31.10, Ninja 1.13.2 and
clang-format 20.1.8. Native commands below use the repository developer wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 <command>
```

| Command (wrapper prefix above) | Result |
| --- | --- |
| `python tools/ci.py --preset windows-debug --jobs 2` | PASS: configure/build and 20/20 CTests, including Core/Server lifecycle and dependency boundaries; logs/toolchain/JUnit in `out/ci/windows-debug/` |
| `cmake --preset graphics-windows-release` then `cmake --build --preset graphics-windows-release --parallel 2` | PASS: supported graphics configuration and updated embedded lock digest |
| `ctest --preset graphics-windows-release --output-on-failure` then `ctest --preset graphics-windows-release --rerun-failed --output-on-failure` | First run 19/20: contract test saw the not-yet-created scope handoff during parallel editing. After its creation, focused rerun 1/1 PASS; all 20 CPU checks now pass. No GPU qualification inferred. |
| `cmake --preset runtime-only` then `cmake --build --preset runtime-only --parallel 2` | PASS: isolated client runtime and transitive boundary |
| `cmake --preset server-only` then `cmake --build --preset server-only --parallel 2` | PASS: isolated Core/Server configuration and boundary |
| `python tools/check_format.py` | PASS: 24 files. An initial direct invocation without the locked tool on PATH correctly rejected the environment; wrapper rerun passed. |

Additional checks used `.tools/venv/Scripts/python.exe` directly:
`tools/check_ci_config.py` PASS (nine jobs), `tools/validate_contract.py --self-test`
PASS (50 requirements, 46 assignments, 20 negative cases), and
`tools/check_repository.py --self-test` PASS (five skill sources and ignore
boundaries). Graphics tool preparation tests pass 18/18 and validation preparation
tests 15/15, also covered by the CPU CTest workflow. `git diff --check` passes.

The current dependency-lock SHA-256 is
`1231da5703cd19d9da35a03e623815fff70f82a8aca72c33421d631c2249fc56`;
the unchanged native probe source SHA-256 is
`9d21bfd2bfd59e51d9737a6361f6a9d2eea6070563d17725c2be50a4ece3fd90`.

GitHub `main` branch protection was updated through
`gh api --method PATCH repos/markdawes08/shark-engine/branches/main/protection/required_status_checks --input out/platform-scope/required-status-checks.json`.
The input retained the previous strict setting and app bindings, filtering only
the six `cpu-macos-*` contexts. Before/after reads of the full protection object
verify all nine retained checks are bound to app 15368, strict freshness remains
enabled and every unrelated returned field is unchanged, including administrator
bypass. The [durable policy receipt](../coverage/evidence/windows-linux-ci-protection.json)
records the request's scope and actual readback. No repository rulesets were present.
No Git staging, commit or push was performed.

Hosted execution of this uncommitted workflow is **NOT_RUN**; it requires the
user's commit/push. Linux native builds, sanitizer reruns, fresh GPU runs and
clean-machine packaging were not performed for this scope/configuration change.
Historical hosted and GPU gates remain evidence only at their recorded revisions.

## Remaining work and exact next task

Continue assignment 02d with the six named Windows/Linux GPU rows and real
clean-target package execution using fresh current-input artifacts. Preserve
the earlier failed/incomplete runs and investigate any reproducible failures.
The earlier unresolved resize timeout remains historical open evidence.
Mac implementation, hardware, CI and distribution work are no longer blockers.
Assignment 03 remains next only after the remaining assignment 02 gate passes
or the user explicitly amends it. This scope amendment does not start 03.
