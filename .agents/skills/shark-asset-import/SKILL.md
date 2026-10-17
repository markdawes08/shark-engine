---
name: shark-asset-import
description: Implement or change Shark asset import and cache behavior with bounded decoding, stable asset identity, explicit format support and licensed offline fixtures.
---

# Shark asset import

Read [AGENTS.md](../../../AGENTS.md),
[decoder/dependency choices](../../../docs/architecture/0002-dependencies.md),
[formats and identity](../../../docs/architecture/0004-formats-and-identity.md)
and the [exact source profile](../../../docs/architecture/0006-v1-profile.md).

Assignment 01 contains no asset database, importer, cache or cooker. Those begin
in 11/12; audio import is 24 and HDRI is 27. There is no import CLI command yet.
Use the existing native CPU checks from [building](../../../docs/building.md)
and `python tools/validate_contract.py` for shared-code/contract changes without
claiming asset behavior was exercised. Add concrete commands and fixtures when
the importing layer lands.

Keep external parsers and image/audio codecs in EngineTools. Runtime loads
validated engine-owned payloads; parser recognition never means rendering or
decompression support. Enforce the required-extension allowlist, core fallback
policy, alpha/color/UV semantics and unsupported-content diagnostics. Reject path
escapes, malformed arithmetic, excessive encoded/decoded sizes and nonfinite data
before publication, with bounded reads/allocations and useful source context.

Asset IDs survive moves and renames. Cache keys include declared source/settings/
tool/dependency inputs; cancellation and failure cannot publish partial results.
Stage and validate output before atomic replacement, preserving the last valid
asset according to the documented policy. Test invalidation, deduplication,
cancellation, rename/move, reproducibility and malformed source paths.

Keep fixtures local and licensed with origin/license metadata; ordinary tests
must not download assets. Cover embedded/external glTF, sparse accessors,
negative/nonuniform transforms, missing images, required extensions and decoder
failures appropriate to the selected profile. Update public version policy,
coverage, FeatureWorld when available, dependency notices and the handoff.
