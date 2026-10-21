# Source document fixtures

These four UTF-8 fixtures were authored directly for assignment 08a and use only
repository-owned test content. They are not encoder snapshots. The version-0
documents intentionally use the legacy flat entity representation, `id`,
`start_scene`, `origin`, `position` and quaternion `rotation_wxyz` fields. The
version-1 files independently describe the same values with the current schemas.

The tests compare explicit typed expectations and both versions, including a child
before its parent, signed nonuniform scale, nonunit quaternion normalization,
Unicode text and paths, the world-cell endpoints, and preserved bounded extras.
Version 0 is an explicit test/legacy format; these fixtures do not claim prior
shipping files existed. Migration is in memory and must leave fixture bytes intact.

Mutation tests construct malformed inputs in memory; fixtures are never rewritten.
File opening here belongs only to the test harness. The 08a production API has no
filesystem access, active Scene, authoring command, or live durable-state behavior.
