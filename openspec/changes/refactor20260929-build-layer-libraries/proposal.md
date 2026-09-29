## Why

P5 extends the delivered six-group source layout into the build graph. The current `acecode_testable` OBJECT target compiles most layers together, while two TUI selection lists split implementations between that target and the executable; this obscures ownership and forces small test helpers to link unrelated surfaces.

## What Changes

- Create STATIC libraries for base_core, base_host, domain, adapters, engine, host, web, TUI and the remaining application assembly code.
- Make `acecode_testable` an INTERFACE aggregate over the same production libraries; remove both TUI subset lists and their selection logic.
- Assign project sources to explicit owners, retain platform-specific helper boundaries and generated assets, and keep Desktop independent of the agent, TUI, Crow and embedded Web assets.
- Update helper consumers, configure-time guards and architecture documentation; verify full MSVC and Linux linking and unchanged C++ test inventory.

## Capabilities

No runtime or public protocol behavior changes. This is the P5 build refactor from `refactor20260927-restructure-src-layers/design.md` section 8.4; `skip_specs: true` records that no behavioral delta is needed.

## Impact

Root and test CMake files, focused CMake modules, build-contract checks and source-layout documentation. No source relocation, provider/protocol changes, P6C file splitting, P7 service redesign, P8 behavior fixes, legacy-ref migration or release publishing. Unrelated working-tree changes are preserved.
