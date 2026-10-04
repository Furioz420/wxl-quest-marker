# Integration source snapshot - 2026-10-04

This repository contains the `wxl-quest-marker` extension maintained under Furioz420.
This update is a source snapshot, not an installable client release.

## Provenance

Source: `50c2f0d82a02216b6e1542eb33b49b617ea41d0b` from the local WXL integration workspace. Existing repository
licenses, attribution, packaging, and independently maintained files are retained.
Uncommitted-source snapshots are identified explicitly and have not been validated
as standalone builds. No game client, database dump, or server credentials are included.

## Build and installation requirements

Build this extension inside a compatible WXL core with the matching extension APIs.
The integration workspace can contain API changes that are not yet in the public
core repository. Existing workflows that select a moving core branch are not proof
of compatibility. Standalone CI and release packaging remain a separate gate:
confirm the required core APIs, pin a compatible public core revision, build, and
validate the matching client installation before publishing a binary release.

This PR does not deploy anything to the game client. Accepted in-game behavior is
evidence for the integrated workspace, not for a separately built DLL from this repo.
