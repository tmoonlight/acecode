## Context

The bundled skill tree lives at `assets/seed/skills` in source builds and under
`share/acecode/seed/skills` in installed builds. Runtime discovery already resolves
that directory, and both the TUI and daemon invoke the seeder before their first
`SkillRegistry` scan.

Today the seeder is gated by `consume_acecode_home_created_by_process()`. It writes
`.seed_skills_state.json` only for a newly created ACECode home, skips every existing
target directory, and hard-codes the same bundle version that is also present in
`MANIFEST.json`. Consequently, existing homes neither receive missing defaults nor
refresh clean ACECode-installed copies.

The seed tree is offline release data. Reconciliation must work on Windows, macOS,
and Linux; preserve user-authored skill content; tolerate process interruption; and
serialize concurrent TUI/daemon startups.

## Goals / Non-Goals

**Goals:**

- Reconcile the current bundled skill set for new and existing ACECode homes.
- Use one monotonic bundle revision to decide whether startup work is required.
- Update only targets whose prior ACECode ownership and unmodified content can be
  proven.
- Make interrupted or concurrent reconciliation retryable without advancing stale
  state.
- Keep seeded skills visible in the same startup after reconciliation.

**Non-Goals:**

- Fetch or update skills from the network.
- Overwrite unknown or user-modified skill directories.
- Turn seed reconciliation into a general skill package manager or repair UI.
- Reconcile on every registry refresh after startup.
- Downgrade user state when an older ACECode executable is launched.

## Decisions

### 1. Use `seed.version` as the canonical monotonic bundle revision

The release bundle adds `assets/seed/seed.version`; installed layouts carry the same
file next to `skills/`. The user marker is `~/.acecode/seed.version`. The initial
value remains `2026-07-20.1`, matching the current manifest.

The accepted format is `YYYY-MM-DD.N`, where the date is fixed-width and `N` is a
non-negative decimal revision. Comparison parses the date and numeric revision
instead of relying on filesystem timestamps or wall-clock freshness. Any seed file
change requires a monotonically newer value.

`MANIFEST.json.bundle_version` remains as descriptive metadata but must match
`seed.version`; tests enforce the mirror. Runtime reads `seed.version`, eliminating
the compiled `kSeedBundleVersion` source.

Alternatives rejected:

- File mtime or age-based TTL would repeatedly reinstall an unchanged old release
  and is unstable across packaging.
- The ACECode application version would force seed work for releases with no skill
  changes.
- Keeping only the C++ constant would retain duplicated release metadata.

### 2. Treat the user marker as “last reconciled”, not “all target bytes equal”

After taking the update lock, startup re-reads both markers. It reconciles when the
user marker is missing, invalid, or older. Equal versions are a no-op; a newer user
marker is a downgrade guard.

A preserved user conflict counts as a completed reconciliation because repeating the
same conflict on every startup provides no value. Missing source files, invalid
bundled metadata, hashing failures, copy failures, or state persistence failures do
not advance the user marker, so the next startup retries.

The marker is written atomically only after the detailed state has been written
successfully.

### 3. Keep detailed ownership in `.seed_skills_state.json`

The existing state file remains the diagnostic and ownership record. A new state
entry records:

- bundle version, source id, name, and relative path;
- reconciliation result (`installed`, `updated`, `unchanged`,
  `preserved_user_modified`, or an error);
- deterministic SHA-256 of the complete source directory;
- deterministic SHA-256 of the installed directory after a successful write.

The tree digest sorts directory and regular-file entries by normalized relative
path and hashes framed entry type, path, and file bytes. This includes empty
directories and supporting files such as `agents/openai.yaml`, unlike the legacy
`SKILL.md`-only FNV digest.

An existing target is updateable only when a previous state entry identifies it as
ACECode-installed or ACECode-updated and its recorded full-tree digest equals the
current target digest. Unknown targets and digest mismatches are preserved.

Legacy entries containing only `skill_md_hash` are conservative:

- a directory containing only `SKILL.md` may be treated as pristine when its legacy
  FNV digest matches;
- a directory with additional files cannot be proven pristine and is preserved;
- absent targets are installed normally.

### 4. Replace pristine seeded directories through staging and rollback

Each missing or updateable seed is copied to a staging directory under the ACECode
home and hashed before publication. Missing targets are published by rename.

For an update, the old target is renamed to a backup sibling, the staged directory is
renamed into place, and the backup is removed only after success. If publication
fails, the old target is restored. Replacing the complete directory also removes
files that no longer exist in the new bundle.

State and version files use the existing `atomic_write_file()` helper. Temporary and
backup paths are confined to the ACECode home and cleaned only while holding the
seed lock.

### 5. Serialize startup reconciliation across threads and processes

A process mutex plus an OS-backed exclusive file lock on
`~/.acecode/.seed_skills_update.lock` protects the complete read/check/reconcile/write
transaction. The implementation follows the existing cross-platform config mutation
lock pattern: `LockFileEx` on Windows and `flock` on POSIX. OS-owned locks release
automatically after process termination, avoiding stale lock-directory recovery.

The second process re-reads the marker after acquiring the lock and normally exits as
an equal-version no-op.

### 6. Replace the first-initialization API with a startup reconciler

`default_skill_seeder` exposes a direct reconciler for tests and a path-resolving
startup wrapper. TUI and daemon call the wrapper after `load_config()` and before
their first registry scan without consuming the home-created flag.

The home-created flag can remain for compatibility with unrelated initialization
hooks, but seed behavior no longer depends on it. Startup reconciliation is
best-effort: errors are logged and do not prevent ACECode from opening.

## Risks / Trade-offs

- **[Risk] Legacy state cannot prove auxiliary files are unmodified** → Preserve
  those directories rather than risk data loss; missing skills still install.
- **[Risk] Directory replacement is not a single atomic operation** → Serialize seed
  writers, stage fully, keep a rollback backup, and run before the local registry
  scan.
- **[Risk] A preserved conflict means the visible skill can remain older** → Record
  an explicit outcome; users can rename/delete their override and remove the marker,
  while a future repair command remains possible.
- **[Risk] Release authors forget to bump the revision** → Test manifest/version
  agreement and document the bump requirement alongside seed hash validation.
- **[Risk] A malformed packaged version disables updates** → Log the error, mutate
  nothing, and leave the user marker unchanged.

## Migration Plan

1. Ship `seed.version` with the current `2026-07-20.1` bundle and keep
   `MANIFEST.json.bundle_version` aligned.
2. On first startup after upgrade, homes without `~/.acecode/seed.version` reconcile
   regardless of whether the home directory already existed.
3. Missing targets are installed. Existing targets are updated only when the prior
   state proves they are pristine; all other targets are preserved.
4. Persist the new full-tree state and then the user marker. Subsequent startups at
   the same bundle revision are no-ops.
5. Rolling back ACECode leaves a newer user marker untouched, preventing an older
   bundle from replacing newer seeded content.

## Open Questions

None. The chosen policy preserves user-modified content and treats explicit conflicts
as reconciled.
