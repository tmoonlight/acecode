# Design

## Context

See proposal.md for motivation. The new installer uses a CMake-built native stub with three Windows resources: package ZIP, uninstaller stub and JSON metadata. Existing release jobs generate both Windows ZIPs but do not build the setup target or collect EXE downloads.

## Goals / Non-Goals

Goals: bind setup assets to the existing release matrix and completeness gate; reuse the same ZIP bytes; verify both architectures without executing ARM64 code on x64.

Non-goals: changing installer UI, introducing signing credentials, replacing the ZIP self-update protocol, or altering older published releases.

## Decisions

- Add an explicit Windows setup build/package step after portable ZIP creation, with the matrix architecture passed to the existing packager. Repacking a directory would produce different bytes and is unnecessary.
- Read embedded resources as data, validate the PE machine, hashes, version and exact ZIP identity, and run an isolated lifecycle smoke test only on native x64. ARM64 execution remains a hardware validation limitation; its PE and payload are inspected.
- Extend the release asset list to nineteen required downloads and collect setup EXEs into SHA256SUMS.txt. Derive installer size budgets from measured stub and ZIP sizes with documented headroom.
- Mirror setup EXEs separately from the updater manifest. Verify both files before deployment and verify public versioned and alias bodies after deployment.
- Reuse ACECODE_SETUP_TEST_ROOT and an isolated USERPROFILE for lifecycle validation; restore process environment and remove only verified temporary paths.
- A reproduced release blocker occurs when silent uninstallation moves its still-running launcher from the installation volume into a different TEMP volume. Put locked-file retirement beside the installation directory instead, and clean those owned retirement directories on subsequent installer/uninstaller runs for that location. Exercise this path with different installation and TEMP drives.

## Risks / Trade-offs

- Windows installers remain unsigned under the current distribution policy; code-signing enrollment is outside this release.
- Resource-only validation cannot prove native ARM64 UI behavior; do not claim an ARM64 runtime test.
- New release assets require publication tooling to recognize EXEs; regression tests cover missing, malformed and oversized installers.

## Migration Plan

Wait for the active installer implementation to finish, integrate the release workflow, run focused and overall checks, commit all intended source through the protected mainline, then create one stable tag. Preserve existing published artifacts if a subsequent mirror check fails and repair the same release.
