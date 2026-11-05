# Prefab source fixture

`beacon-v1.sharkprefab` is an independently authored MIT-licensed Shark fixture.
Its child precedes the root to exercise forward parent references. The expected
values are asserted directly in `tests/prefab_document_tests.cpp`.

The mesh and script references are source values only. Decoding this fixture does
not import meshes, read scripts, create physics bodies or instantiate a scene.
The `prefab` key inside extras is an opaque annotation, not a nested prefab.
