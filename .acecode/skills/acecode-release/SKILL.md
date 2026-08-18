---
name: acecode-release
description: Release ACECode or publish a fast Windows prerelease validation package. Use when asked to publish an ACECode version, update the Windows self-upgrade server, create a Jenkins Green pre package, build and verify updater packages, commit release code, create an annotated Git version tag, or push release commits/tags.
platforms: [windows]
compatibility: ACECode skill system
metadata:
  tags: [release, packaging, windows]
---

# ACECode Release

## Purpose

Release ACECode in a traceable way: version files, tests, Git commit, annotated tag, Windows updater zip, `aceupdate.json`, and HTTP verification must all agree on the same version.

Use `scripts/publish_acecode_release.ps1` for the mechanical work whenever possible. Resolve that script relative to this skill directory, not a Codex or other-agent path.

Agent Browser is integrated into `acecode-desktop.exe`. Release packages must not contain legacy `ace-browser-host` or `ace-browser-bridge` artifacts, even when stale files remain in the build tree.

## Required Inputs

For a stable release, require a numeric semantic version such as `0.2.2` and treat the Git tag as `v<version>`.
For a quick validation package, omit the version to auto-select the next `x.y.z-pre.N`, or provide that exact form explicitly.
When publishing a stable release to `aupdate`, also require a concise customer-facing upgrade tip.
The tip may contain Chinese text and multiple lines; pass it with `-UpgradeTip`.
Runs with `-NoPublish` do not require a tip.

Default repository and update service assumptions:

- Repo root: current working directory, normally the ACECode checkout
- Update server folder: `J:\jenkins_green\aupdate`
- Update HTTP base URL: `http://2017studio.imwork.net:82/aupdate/`
- Windows update target: `windows-x64`

## Quick Windows Validation Package

When the user asks for a quick validation package, a pre package, or a Jenkins Green Windows test package, use `-QuickValidation`. This is an internal packaging path, not a release:

- Build the current working tree, including intended uncommitted changes.
- Build only `acecode` and `acecode-desktop`; skip unit-test compilation and execution for speed.
- Publish only `acecode-<version>-windows-x64.zip` to `J:\jenkins_green\aupdate` and update `aceupdate.json` with one `windows-x64` package entry.
- Do not create or update a browser-extension, Linux, macOS, npm, GitHub Release, Git commit, Git tag, or Git push artifact.
- Temporarily inject the full `x.y.z-pre.N` executable version while keeping CMake's numeric version valid, leave `vcpkg.json` untouched, then restore `CMakeLists.txt` and `src\version.hpp.in` even if the run fails.
- Use a new prerelease number for each tester-visible update. When `-Version` is omitted, derive the next version from the highest stable version and prior numeric `pre.N` records in `aceupdate.json`.
- Default the note to an internal prerelease message; pass `-UpgradeTip` only when a more specific test description is useful.

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -QuickValidation `
  -Repo .
```

With stable `0.8.6` and no existing `0.8.7-pre.N` record, this publishes `0.8.7-pre.1`. A later stable release should normally use the same numeric core, `0.8.7`.

## Seed Upgrade Compatibility (Required)

Treat `assets\seed` as migration-bearing release content, not as static files that are correct merely because they appear in the zip. Existing users may already have `~/.acecode/seed.version`, `.seed_skills_state.json`, missing managed resources, or user-modified copies. Every stable release and quick validation package must consider that upgrade state.

Before packaging:

- Inspect changes under `assets\seed`, plus `assets\seed\seed.version`, `assets\seed\MANIFEST.json`, `src\skills\default_skill_seeder.cpp`, and the corresponding seeder tests. A feature that adds or changes a bundled Skill, expert, hook, ownership rule, trust rule, or reconciliation behavior must use a new monotonically increasing seed revision.
- Keep the seed revision, manifest bundle version, managed source IDs, official fingerprints, package metadata, and tests synchronized. Do not publish a package in which the new resource exists but the seed marker still permits an already-initialized user to skip it.
- Preserve user-modified or unknown resources, never downgrade a newer user marker, and write the user's new marker only after reconciliation and state persistence complete successfully.
- For managed resources whose absence should self-heal, especially default hooks, handle both an older user marker and an equal marker with a missing target. Recognized previous official definitions may be upgraded; unknown or user-modified definitions must not be overwritten or automatically trusted.

When a release can affect seed contents or reconciliation, validation is mandatory even though the quick-release script itself skips the broad unit suite:

1. Run focused seeder and registry tests covering clean install, old-marker upgrade, equal-marker missing-resource repair, recognized previous-official upgrade, user-modification preservation, failure/marker behavior, and `ManagedTrusted` loading where applicable.
2. Build the final package, extract that exact zip to a temporary directory, and run its packaged `acecode.exe` with an isolated `USERPROFILE`. Seed the temporary `~/.acecode` from a real prior package marker, leave the new managed resource absent, invoke a command that performs startup reconciliation such as `--validate-models-registry`, and verify the resource, marker, and `.seed_skills_state.json` outcome.
3. Repeat with the packaged marker already equal to the user's marker while the self-healing managed resource is absent. This catches the equal-version early-return failure that can otherwise survive every later startup.
4. Inspect the zip itself for `share\acecode\seed\seed.version`, `MANIFEST.json`, and every intended managed resource; verify their contents and fingerprints agree with the source tree and tests.
5. For an external integration hook, validate the final packaged executable against the real integration when available, not only a fake command. Use an isolated profile and close only the exact temporary tab, pane, or process created for the check.

Report the upgrade semantics explicitly: seed reconciliation happens on the next ACECode startup after installation, so already-running ACECode processes need to be restarted or replaced with a new pane. Do not call the release complete based only on package presence, source-tree tests, or a clean-home install.

## Stable Release Workflow

1. Reconcile the real local `master` before changing versions or creating a tag. A request to release ACECode implicitly authorizes committing and pushing the usable work on local `master`, plus the release commit and tag, unless the user explicitly asks for a local-only or no-push release.
   - Run `git fetch origin`, `git status --short --untracked-files=all`, `git rev-list --left-right --count origin/master...master`, and `git log --oneline origin/master..master` in the canonical `master` worktree.
   - Inventory `git worktree list --porcelain` and recent local commits with `git log --all --not --remotes`. Use reflogs when a rebase or detached worktree may have hidden a commit. When old hashes are pre-rebase versions, prove patch equivalence with `git range-diff` or `git cherry` instead of applying them twice.
   - Treat all non-generated, non-secret tracked and untracked changes in the `master` checkout, and every commit on local `master` that is absent from `origin/master`, as intended release content. Commit it, integrate the latest `origin/master`, and push it before tagging. Never silently package an older clean commit while `master` has dirty or outgoing work.
   - Inspect recent dirty or unmerged worktrees so an intended feature is not omitted merely because it was developed outside the canonical checkout. Include clearly intended release work; warn when ownership is genuinely ambiguous rather than silently excluding it.
   - Exclude generated build outputs, caches, diagnostics, and secrets. Stop and warn only when a feature is unusable, conflicts cannot be resolved safely, validation fails, or committing would expose secrets or generated junk. A usable dirty feature is work to finish and include, not a reason to preserve it outside the release.
2. If publishing a stable release after a successful prerelease or packaging-fix validation, verify the validated commit is on the release branch before versioning:
   - Use `git merge-base --is-ancestor <validated-tag-or-commit> HEAD`.
   - If it fails, do not release from `master` yet; fast-forward/merge the validated branch first, then re-check.
   - This prevents a branch-only prerelease such as a package-size experiment from being skipped by a later stable release.
3. Before packaging, prove the release `HEAD` descends from the reconciled `master` content and the latest `origin/master`. Run the applicable feature tests, complete the required seed-upgrade validation above when relevant, and confirm the canonical `master` worktree is clean. Seed fixes validated only from a dirty quick package must be committed on this release `HEAD` before tagging.
4. Write a short user-visible upgrade tip that explains features and fixes in product language, then run the release script with the version, `-UpgradeTip`, and explicit `-StageFiles` for any remaining release-code changes beyond `CMakeLists.txt` and `vcpkg.json`.
   - If the release changes packaging, include `.github\workflows\package.yml` and any release script/skill edits in the intended file list or keep them committed before publishing.
5. Use `-Push` by default for an ACECode release request. Omit it only when the user explicitly requests a local-only or no-push release.
6. After the script finishes, verify `master` and `origin/master` have no divergence, `git log origin/master..master` is empty, the release tag points to the final included commit, and all expected GitHub/update-server assets came from that tag. Then report the commit/tag, package paths, package sizes, SHA256 values, and verification commands. Also state that Agent Browser is integrated into Desktop and the updater zip contains no legacy browser host or extension artifact.

Typical command:

```powershell
$upgradeTip = @'
1. 检查更新时可查看各版本的更新说明。
2. 优化升级流程的稳定性。
'@

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -Version 0.2.2 `
  -Repo . `
  -UpgradeTip $upgradeTip `
  -StageFiles main.cpp,src\upgrade\http.cpp,src\upgrade\http.hpp,src\upgrade\upgrade.cpp,tests\upgrade\upgrade_http_test.cpp `
  -Push
```

If the version bump and all release changes are already committed, run without `-StageFiles`; the script will tag the current `HEAD` and package the current Release build after verification.

## Script Behavior

In stable release mode, the script:

- Updates `CMakeLists.txt` `project(acecode VERSION ...)`.
- Updates `vcpkg.json` `version-semver` when present.
- Refuses to build/publish if there are dirty files outside the explicit release file set, unless `-AllowDirtyBuild` is passed.
- Builds `acecode`, `acecode-desktop`, and `acecode_unit_tests` in Release.
- Runs `Upgrade*:*ConfigUpgrade*` tests unless `-SkipTests` is passed.
- Verifies `build\Release\acecode.exe --version` outputs the requested version.
- Commits staged release files unless `-NoCommit` is passed.
- Creates annotated tag `v<version>` unless `-NoTag` is passed.
- Optionally pushes `HEAD` and the tag when `-Push` is passed.
- Creates `acecode-<version>-windows-x64.zip` under the update server folder.
- Packages Agent Browser as part of `acecode-desktop.exe` and rejects legacy `ace-browser-*` entries in the Windows updater zip.
- Ensures update-server `web.config` maps `.zip` to `application/zip`.
- Requires a non-empty `-UpgradeTip` before any publishing side effects.
- Updates `aceupdate.json`, preserving older release records, putting the new release first, and writing the trimmed tip to `releases[0].notes`.
- Verifies the manifest tip and zip over HTTP, including `Accept: application/zip`.

In quick validation mode, the script:

- Auto-selects the next numeric `pre.N` version unless `-Version x.y.z-pre.N` is provided.
- Rejects stable versions, non-Windows targets, `-Push`, `-NoPublish`, `-StageFiles`, and `-CommitMessage`.
- Accepts a dirty working tree by design, reports its paths, and never stages or commits it.
- Builds only the two Windows runtime targets and verifies `acecode.exe --version` reports the full prerelease version.
- Writes only the Windows zip plus the required `aceupdate.json` and `web.config` update-server metadata.
- Preserves existing release records while inserting a prerelease record containing only `windows-x64`.
- Verifies the Windows package and manifest over HTTP, then restores all temporarily changed source-version files.

After changing this release tooling, run `scripts\test_quick_validation.ps1` for the isolated package/rollback regression test.

## Guardrails

- Do not run `git reset --hard` or revert unrelated files.
- For stable releases, do not package from a dirty working tree unless the user knowingly accepts `-AllowDirtyBuild`. Quick validation intentionally packages the current working tree without committing it.
- Do not call a release complete while the canonical `master` worktree has usable uncommitted changes or local `master` has commits absent from `origin/master`.
- Do not interpret preservation of dirty work as release correctness. For a release request, finish, validate, commit, integrate, and push usable `master` work by default.
- Do not reuse an existing tag unless the user explicitly requests manual tag repair; the script fails on existing tags.
- Do not publish a generic package sentence as the upgrade tip; use short customer-facing feature and fix language.
- Keep `aceupdate.json` version, zip filename, SHA256, package size, and `acecode.exe --version` consistent; stable releases must also match the Git tag.
- Never turn quick validation into a partial release: do not add `-Push`, create a tag, publish GitHub assets, or mirror non-Windows packages.
- Remember that publishing a prerelease to the shared Jenkins Green manifest makes it visible to clients using that update service. Use the next `pre.N`, not an existing version, when testers must receive another update.
- Inspect the reported dirty paths before quick packaging and exclude secrets or private machine artifacts from compiled resources and packaged asset folders.
- Treat a quick package built from dirty files as ephemeral manual-test evidence, not as proof that a later stable `HEAD` contains the validated code. Commit and integrate the exact changes, then rerun the applicable validation before the stable release.
- Remember that users upgrading from older binaries see the old updater UI during the first upgrade; new UI changes are visible after the upgraded binary is installed.
- Never infer that existing users receive a new seeded Skill, expert, or hook merely because it is present under `share\acecode\seed` in the package. Prove the old-marker and equal-marker upgrade paths with the final packaged executable, and state the required restart behavior in the handoff.
- Treat successful tagged or committed prerelease points as validation evidence, not as automatically released code. Before a stable release, prove the validated prerelease or package-fix commit is an ancestor of the stable release commit.
- Keep package-size protections in source control, preferably as CI checks, so they fail before release rather than relying on memory:
  - `.github/workflows/package.yml` package builds must use `MinSizeRel`, not `Release`.
  - Linux packages must strip runtime binaries with `strip --strip-unneeded`, not only `--strip-debug`.
  - macOS packages must strip the desktop app executable and bundled `acecode-daemon` after creating dSYM artifacts.
  - `acecode-desktop` must not link the broad `acecode_testable` object set or FTXUI; use focused desktop/native support targets so the desktop shell does not pull agent/TUI/web/server/provider code.
  - CI should enforce package size budgets for release assets and fail on regressions. Calibrate budgets from the last known-good release assets, then update intentionally with evidence when size growth is expected.
- When deploying non-Windows packages to `J:\jenkins_green\aupdate`, keep in mind the current self-updater extracts zip packages only. Publish `.tar.gz` files and latest aliases as downloadable assets, but do not add them to `aceupdate.json` `packages` until Unix/macOS updater extraction is implemented and tested.
