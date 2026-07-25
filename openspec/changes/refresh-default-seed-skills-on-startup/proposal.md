## Why

ACECode currently installs bundled default skills only when the current process creates
`~/.acecode`, so existing users never receive newly added or refreshed seed skills.
The bundle already carries version metadata, but startup does not compare it with any
applied-version marker in the user profile.

## What Changes

- Add a canonical `assets/seed/seed.version` revision for the complete bundled skill
  set and persist the last reconciled revision as `~/.acecode/seed.version`.
- Reconcile bundled skills on TUI and daemon startup before the first skill registry
  scan whenever the user marker is missing, invalid, or older than the bundled
  revision.
- Install missing bundled skills and update only copies that ACECode can prove are
  still unmodified; preserve unknown or user-modified skill directories and record
  the conflict instead of overwriting them.
- Record full seeded-directory hashes and per-skill reconciliation outcomes in the
  existing seed state, advance the user version marker only after a valid bundle has
  been reconciled, and retry recoverable failures on the next startup.
- Treat a newer user marker as a downgrade guard, keep reconciliation offline, and
  make startup updates safe across concurrent TUI/daemon processes.
- Update the default-skill specification and documentation from first-run-only
  installation to versioned startup reconciliation, including the current eight-skill
  bundle.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `default-global-skills`: replace first-initialization-only seeding with versioned,
  ownership-aware startup reconciliation while preserving user-modified content.

## Impact

- Seed assets and packaging: `assets/seed/seed.version`,
  `assets/seed/MANIFEST.json`, and the existing CMake seed-directory install rule.
- Runtime: `src/skills/default_skill_seeder.*` plus the shared TUI and daemon startup
  call sites.
- State: `~/.acecode/seed.version` and the backward-compatible
  `~/.acecode/.seed_skills_state.json`.
- Verification and documentation: `tests/skills/default_skill_seeder_test.cpp`,
  `docs/skills.md`, and the `default-global-skills` OpenSpec capability.
- No network dependency, config-schema change, or automatic overwrite of
  user-modified skills.
