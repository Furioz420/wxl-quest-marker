# Building wxl-quest-marker

Build this extension with the public `Furioz420/wxl-core` commit
`48b2849ed05d2c66e2ba2a6185e09fafd111da53` (Win32, client build 12340). The GitHub Actions workflow pins
that exact core revision rather than a moving `v1.1` branch. Core owns the
`wxl/*.h` extension SDK and its runtime bindings; this module owns its
headers under `src/`. Do not copy SDK headers into the module.

The workflow builds on pull requests and `main`; it publishes a package only from an explicit `v*` tag after client validation.

A local clean-checkout MSVC Win32 build produced `wxl-quest-marker.dll` against
the pinned core source. This is a compile/link check, not a claim that
a separately packaged DLL has passed an in-game smoke test.

The module's existing source-file notices and license remain in force.
