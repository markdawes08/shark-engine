# Independent glTF fixture validator

Assignment 12e uses the official [Khronos glTF Validator](https://github.com/KhronosGroup/glTF-Validator)
as an additional offline oracle for the original licensed fixtures. It does not
replace Shark's source, geometry, texture, material, payload or cache tests and
does not establish rendering support. The validator is an opt-in developer tool,
outside engine dependencies, CMake targets, ordinary CTests and shipped packages.

The [official Node package](https://github.com/KhronosGroup/glTF-Validator/blob/v2.0.0-dev.3.10/node/README.md)
contains a precompiled Dart-to-JavaScript validator and has no npm dependencies.
The existing local Node installation is sufficient; no global install, npm
install, host setting change or runtime dependency is needed.

| Input | Exact choice |
| --- | --- |
| Package | `gltf-validator`, `2.0.0-dev.3.10` |
| Archive | `https://registry.npmjs.org/gltf-validator/-/gltf-validator-2.0.0-dev.3.10.tgz` |
| Archive SHA-256 | `4e03dbdc3bc0d1342afd5d2d7ae341a7e3474502ccb872b02dbbaddaee0fdec6` |
| Registry SHA-512 integrity | `sha512-odJ4k0tRkGXiDGn78yDBg+fBbAIvBnXxh3RwAta0emSxGtyagFE8B4xELB1oYe3S5RD8Ci3uZAsZaascH2LAEQ==` |
| License | Apache-2.0; retain the archive's complete `LICENSE` and bundled `NOTICES` |
| 12e execution | Node `v22.17.1`, native Windows |

Prepare explicitly once from the repository root. This is the only network step;
the check itself never downloads or resolves network resources. Reuse the cached
archive and extracted package on later runs.

```powershell
New-Item -ItemType Directory -Force out/dependency-cache | Out-Null
Invoke-WebRequest -Uri 'https://registry.npmjs.org/gltf-validator/-/gltf-validator-2.0.0-dev.3.10.tgz' -OutFile 'out/dependency-cache/gltf-validator-2.0.0-dev.3.10.tgz'
@'
import hashlib, pathlib, tarfile
archive = pathlib.Path('out/dependency-cache/gltf-validator-2.0.0-dev.3.10.tgz')
expected = '4e03dbdc3bc0d1342afd5d2d7ae341a7e3474502ccb872b02dbbaddaee0fdec6'
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('Validator archive digest mismatch')
with tarfile.open(archive) as package:
    package.extractall('out/dependency-cache/gltf-validator-2.0.0-dev.3.10', filter='data')
'@ | .tools/venv/Scripts/python.exe -
node tools/check_gltf_oracle.cjs out/dependency-cache/gltf-validator-2.0.0-dev.3.10/package out/12e-work/oracle/final
```

[The helper](../../tools/check_gltf_oracle.cjs) checks the exact executable module,
package metadata and license/notice hashes before loading the tool. External
resources are bounded local reads within the fixture's directory; their returned
`Uint8Array` owns a zero-offset buffer. Passing pooled Node buffers directly to
this validator's external-resource bridge can expose the wrong byte offset.
There are no issue suppression or severity overrides.

The twelve saved source/geometry/texture `.gltf`/`.glb` inputs cover external,
base64 and binary-chunk storage, sparse values and repaired indices, shared
strips, negative/nonuniform transforms, PNG references and core texture roles.
Three in-memory controls require named errors for an oversized accessor, a
missing image and an inconsistent GLB length. Reports retain all issues; only
the documented generated-tangent warning is accepted on positive fixtures.

The initial 12e cross-check found missing required POSITION `min`/`max` metadata
in the three texture containers. Their generator and saved containers now carry
the exact triangle bounds `[0,0,0]` through `[1,1,0]`; source buffers, image bytes,
independent expected pixels and test workloads are unchanged. The existing
fixture manifest records the corrected container hashes. Original failing
validator reports remain under `out/12e-work/oracle/`; final reports and the
15-result summary are under `out/12e-work/oracle/final/`.

Final result: all twelve positive inputs have zero errors. The three texture
inputs retain `MESH_PRIMITIVE_GENERATED_TANGENT_SPACE` warnings, consistent with
Shark's documented generated tangent basis; other messages are buffer-target
optimization hints. All three controls produce their expected error. This
validator does not qualify Shark budgets, optional-extension fallback policy,
pixel decoding/mips, publication fences or its development payload; engine tests
remain the authority for those behaviors.
