# Integration source snapshot - 2026-10-04

This repository contains the `wxl-quest-marker` extension maintained under Furioz420.
This update is a source snapshot, not an installable client release.

## Provenance

Source: `50c2f0d82a02216b6e1542eb33b49b617ea41d0b` from the local WXL integration workspace. Existing repository
licenses, attribution, packaging, and independently maintained files are retained.
Uncommitted-source snapshots are identified explicitly and have not been validated
as standalone builds. No game client, database dump, or server credentials are included.

## Build and installation requirements

The source build now pins Furioz420/wxl-core commit
`48b2849ed05d2c66e2ba2a6185e09fafd111da53`, which includes the extension
SDK and per-module include path. A clean MSVC Win32 build produced this module's
DLL against that core revision. GitHub CI and matching-client smoke tests remain
release gates; a successful source build is not an installable-client acceptance.

This PR does not deploy anything to the game client. Accepted in-game behavior is
evidence for the integrated workspace, not for a separately built DLL from this repo.
