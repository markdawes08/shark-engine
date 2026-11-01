# 19a: private Lua VM foundation

Date: 2026-11-01. Base: `16a0f63`, initially clean `main`. The user owns
staging, commits and pushing. 19a adds the internal foundation described by
the [Lua contract](../lua-vm.md) and [checkpoints](../roadmap/lua-plan.md).
Full REQ-19 remains NOT_RUN: component persistence, lifecycle callbacks,
gameplay APIs and human/terminal execution are later work.

EngineCore privately owns a creating-thread Lua VM. It validates bounded UTF-8
source and names, meters its allocator, executes in protected calls with a
fresh instruction limit and produces bounded owned diagnostics and tracebacks.
It isolates VM state and remains usable after tested syntax, runtime, allocation
and instruction failures; execution is not transactional. No native handle,
Scene, gameplay authority or public scripting endpoint is exposed. The profile
restricts standard libraries to prevent hook-error catching and script-defined
finalizers; coroutine support requires a later bounded implementation. Hooks do
not meter native library work or impose wall-time limits. This is not a complete
hostile-code sandbox.

Official Lua 5.5.1 is locked to archive SHA-256
`1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.
The private C target builds only the explicitly reviewed source list and receives
sanitizer instrumentation. Offline population checks the archive and complete
source inventory; Runtime and Server package their exact upstream MIT notice.
Existing disconnected configure fixtures and the headless package helper carry
the same locked archive/notice. No public component, Lua API entry, FeatureWorld
case, source format or protocol version is introduced; the Lua manifest remains
empty until bindings actually land.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): VM behavior and
allocator limits, affected offline dependency/notice tooling, portable WSL and
one sanitizer, plus protected Runtime and Server closures. No whole-project test
matrix or scripting gameplay acceptance is claimed. Outputs are under
`out/19a-work/`.

| Check | Outcome |
| --- | --- |
| Windows Debug `shark.core.lua` and `shark.tooling.lua-dependencies` | PASS 2/2, 1.22 s; `windows-debug-final.xml`. The VM family has eight cases and 203 assertions. |
| WSL Ubuntu GCC Debug, same selection | PASS 2/2, 0.33 s; `wsl-final/linux-debug-tests.log/.xml`. |
| WSL Ubuntu Clang ASan+UBSan, same selection | PASS 2/2, 0.61 s; `wsl-final/linux-asan-ubsan-tests.log/.xml`. Both Lua C and the Core owner are instrumented. |
| Protected Windows Release Runtime | Build/boundary PASS. Package assembly and inspection PASS with 36 files, including Lua notice. Standard-user relocated package launch/module inspection PASS on both local GPUs with validation off, `runtime-isolated-verified/summary.json`; the graphics-sample launcher uses the package directory as its working directory. This is deployment evidence, not rendering qualification or a fresh scene-export launch from a separate working directory. |
| WSL Release Server | Build, install and strict source/target/link/package/notice closure PASS; `wsl-final/server-closure.json` and related logs. The installed Server started and stopped normally in the retained SDK-free Ubuntu root with a read-only package and isolated network, using `server-clean.sh`. |
| Runtime package tooling | 33 existing/new unit cases PASS; the specific system-converter and forbidden compiler/module cases pass after the narrow audit correction. |
| Server closure tooling | 52 cases passed initially; the remaining expected source-count assertion was updated for the 26 reviewed Lua C files and its targeted rerun passed. |
| Offline configure fixtures | Selected scene archive/population and Lua rejection cases PASS; selected Server storage, independent CPU authoring and device-free client/headless configurations PASS. Detailed commands and outcomes: `offline-fixtures.log`. |
| Repository checks | Contract/link/coverage validation, skill/ignore validation, pinned clang-format and `git diff --check` PASS. `.gitignore` already covers the local roadmap and generated outputs. |

The Windows selection used `tools/windows_dev.ps1 cmake --build --preset
windows-debug --target CoreLuaTests --parallel 2` followed by `ctest --preset
windows-debug -R 'shark\.(core\.lua|tooling\.lua-dependencies)$'
--output-on-failure --no-tests=error`. The retained WSL driver and selected
configure commands are in `out/19a-work/wsl-final-commands.sh` and
`out/19a-work/offline-fixtures.py`; the latter obtains each registered command
and runs only the named affected fixture methods.

The first expanded WSL sanitizer build failed on a new Catch2 assertion whose
boolean expression needed extra parentheses; the test source was corrected and
both final WSL profiles passed. The initial isolated Runtime run rejected
Microsoft's signed `System32/dxilconv.dll` by a broad name rule, despite the
RTX launch itself completing. The audit now accepts only that exact Windows
system converter and still rejects `dxcompiler.dll`, `dxil.dll`, other converter
locations and unexpected package files. The original failure remains in
`runtime-isolated-final/`; the fresh standard-user two-device run passed.
Earlier elevated/ACL setup attempts and their diagnostics are not passes.
The first isolated Server command passed a CRLF-terminated duration and was
correctly rejected; converting the helper to LF yielded the clean launch above.

The next checkpoint is 19b: Script component persistence and shared authoring.
Continue to use the existing Scene/authoring owners, checked revisions and
source-file tokens. Do not claim runnable scripts in the Editor or terminal
until lifecycle and connected execution actually land.
