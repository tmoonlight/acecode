---
name: acecode-release
description: Release ACECode by committing and pushing all pending code, merging completed worktrees from the last two days, pushing the integrated result, and publishing one verified stable version. Also supports GitHub-to-aupdate mirroring and fast Windows prerelease validation packages. Use when asked to release ACECode, integrate completed worktrees for a release, repair the update server, or publish a Jenkins Green pre package.
---

# ACECode Release

## Purpose

默认正式发布顺序：提交所有未提交代码并 push → 合并近两天已完成的 worktree → 再次 push → 最终统一发布一个版本。发布请求即授权完成这整条流程，无需逐步重复询问是否提交、推送、合并或发版；用户明确指定的范围、版本和例外优先。仅修改本 skill、检查发布状态或同步已有版本的镜像，不触发这条发布流程。明确要求快速 Windows 验证包时，使用下方独立的快速流程。

Release ACECode in a traceable way: version files, tests, Git commit, annotated tag, GitHub Release assets, all stable updater ZIPs, both macOS PKG installers, `aceupdate.json`, and public HTTP verification must agree on the same version. A full stable release is not complete while required updater ZIPs or either macOS PKG exist only on GitHub or are missing.

Use `scripts/publish_acecode_release.ps1` for the mechanical work whenever possible. Resolve scripts relative to this skill directory. The repository scripts retain build-directory selection, build locking, Windows Computer Use and WhatsApp payload checks, and exact public manifest verification.

Agent Browser is integrated into `acecode-desktop.exe`. Release packages must not contain legacy `ace-browser-host` or `ace-browser-bridge` artifacts, even when stale files remain in the build tree.

## Required Inputs

For a stable release, use a numeric semantic version such as `0.2.2` and treat the Git tag as `v<version>`. Honor an explicit version. Otherwise inspect fetched tags, source version files, and the update manifest, choose the next unused patch version above the latest stable version (or the higher unreleased numeric core already prepared for this release), and state it before publishing without requiring a separate version-selection round trip.
For a quick validation package, omit the version to auto-select the next `x.y.z-pre.N`, or provide that exact form explicitly.
When publishing a stable release to `aupdate`, also require a concise customer-facing upgrade tip. Derive it from the complete integrated changes unless the user supplies one.
The tip may contain Chinese text and multiple lines; pass it with `-UpgradeTip`.
Runs with `-NoPublish` do not require a tip.

Default repository and update service assumptions:

- Repo root: current working directory, normally `N:\Users\shao\acecode`
- GitHub repository: `tmoonlight/acecode`
- Update server folder: `J:\jenkins_green\aupdate`
- Update HTTP base URL: `http://2017studio.imwork.net:82/aupdate/`
- Windows update target: `windows-x64`

## Quick Windows Validation Package (Speed First)

When the user asks to "发快速版本", publish a quick validation package, a pre package, or a Jenkins Green Windows test package, treat that request as authorization for this short, speed-first path:

1. Finish the intended source change on the canonical `master` checkout, commit it, and push `master`. If there is no source change, reuse the current `HEAD` instead of creating an empty commit. Inspect only the concise status needed to avoid committing generated output, secrets, or unrelated work; do not perform a broad worktree/reflog audit for an ordinary quick-version request.
2. Immediately before any Web or C++ compilation, find running processes whose exact process names are `acecode`, `acecode-desktop`, or `acecode-daemon` and force-stop them without asking for a second confirmation. This standing authorization applies specifically to quick-version requests. Do not use substring matching and do not stop compilers, shells, Codex, or unrelated processes.
3. Build in the canonical checkout and reuse its configured build directory and incremental caches. Do not create an isolated worktree merely because an ACECode binary is locked; stop the ACECode process and build in place. Rebuild `web\dist` first only when Web source/build inputs changed or the embedded assets are missing.
4. Run `scripts/publish_acecode_release.ps1 -QuickValidation` to compile only `acecode` and `acecode-desktop`, copy the Windows x64 zip into `J:\jenkins_green\aupdate`, update `aceupdate.json`, and complete the script's package/HTTP checks.
5. Report the commit, prerelease version, package path, public URL, size, and SHA256. Do not add slow duplicate validation after the script succeeds unless seed/packaging behavior changed or the build/publish output indicates a problem.

The quick-package boundary remains narrow:

- Publish only `acecode-<version>-windows-x64.zip` to `J:\jenkins_green\aupdate` and update `aceupdate.json` with one `windows-x64` package entry.
- Do not create or update a browser-extension, Linux, macOS, npm, GitHub Release, release commit, or Git tag. The source commit and `master` push in step 1 happen before `-QuickValidation`; the script itself must still receive no `-Push`, `-StageFiles`, or commit option.
- Temporarily inject the full `x.y.z-pre.N` executable version while keeping CMake's numeric version valid, leave `vcpkg.json` untouched, then restore `CMakeLists.txt` and `src\version.hpp.in` even if the run fails.
- Use a new prerelease number for each tester-visible update. When `-Version` is omitted, derive the next version from the highest stable version and prior numeric `pre.N` records in `aceupdate.json`.
- Default the note to an internal prerelease message; pass `-UpgradeTip` only when a more specific test description is useful.

Use exact-name process termination before compiling:

```powershell
$acecodeProcesses = Get-Process -Name acecode,acecode-desktop,acecode-daemon -ErrorAction SilentlyContinue
if ($acecodeProcesses) {
  $acecodeProcesses | Stop-Process -Force
  $acecodeProcesses | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
}
```

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -QuickValidation `
  -Repo N:\Users\shao\acecode `
  -Configuration MinSizeRel
```

To validate release arguments, the selected build directory, and the dynamically
calculated parallelism without building, packaging, touching Git, or contacting
the update server, add `-DryRun` (optionally with `-BuildJobs 3`). For quick
validation, `-DryRun -NoPublish` is accepted because no publish occurs:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -QuickValidation -Version 0.9.13-pre.1 -Repo . `
  -DryRun -NoPublish -BuildJobs 3
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

1. **盘点并固定本轮范围。** 在发布开始时记录当前时间，以用户时区（默认 `Asia/Taipei`）的最近 48 小时作为“近两天”；后续操作沿用这个截止窗口。
   - 在 canonical `master` 中执行 `git fetch origin --tags`、`git status --short --untracked-files=all`、`git rev-list --left-right --count origin/master...master` 和 `git log --oneline origin/master..master`，同时用 `git worktree list --porcelain` 盘点整个仓库的 worktree，并逐个检查分支、HEAD、未提交文件和未推送提交。
   - 候选包括近 48 小时创建、发生开发活动或完成的 worktree；较早创建但近两天完成的也纳入。通过关联聊天/任务的完成记录、OpenSpec 任务与验收记录、Git 日志和 reflog 核实时间与状态。提交时间或文件夹修改时间只能用于发现候选，不能单独证明已完成。
   - “已完成”必须有任务完成及适用验证的依据，且没有仍在写入该 worktree 的活动任务。已完成但尚未 commit 的代码同样属于本次范围。记录每个候选的路径、分支、HEAD、完成依据及纳入/已合并/未完成/排除原因；无法确认的明确报告，不能假定完成或静默遗漏。
   - 遵守仓库明确保留的 ref/worktree：src 分层搬迁记录中的九个旧 ref/worktree 继续保留原状，迁移另行安排，不因最近有活动自动纳入。其他正在开发且未完成的 worktree 保留并列明。
2. **提交所有未提交代码并先 push。** 默认把当前检出、canonical `master` 和本轮已完成 worktree 中的全部代码、测试、文档、配置变更作为发布内容，包括已跟踪文件的修改/删除及应纳入版本控制的新文件；不按是否由本聊天产生筛选。当前检出若仍有活动任务，先等其完成再取稳定快照。
   - 审查差异并排除密钥、个人运行配置、会话数据、缓存、构建产物和临时诊断文件；正常维护的仓库资源不因由生成器产生就自动排除。按明确文件清单暂存，完成适用检查，提交到各自所属分支并 push。没有改动时不创建空提交；已有但未推送的提交也要推送。
   - 工作区中的可用改动需要完成、验证并纳入，不能以“保留他人改动”为由丢在发布之外；不得提交其他任务仍在写入的中间状态。暂存、提交前重新核对差异，有并发变化时重新检查对应内容。
   - 同步最新远端再正常推送，不 force-push。`master` 受保护时，通过必要的临时集成分支和 PR 完成提交入主线，等待所需检查及合入，再同步 canonical `master`；这也适用于后续 worktree 集成和版本提交。除此之外复用现有检出。
3. **合并近两天已完成的 worktree。** 在第一轮提交与 push 成功后，按依赖顺序把候选中的已完成分支逐个合入 canonical `master`。第 2 步已纳入的当前检出若位于其他分支，其发布提交也必须合入，不受两天窗口排除。先用 `git merge-base --is-ancestor` 判断是否已包含；rebase、squash 或 cherry-pick 导致哈希变化时，用 `git range-diff`、`git cherry` 或实际差异核验补丁等价，避免重复应用。
   - 审查每个分支的完整新增提交及其依赖，解决冲突时保留双方有效行为；旧布局分支遵守仓库的路径迁移和 `migrate_branch.py --check` 要求。常规冲突自行解决并验证，无法判断产品意图时才说明具体阻塞并询问。
   - 合并后运行适用的定向检查，记录来源 HEAD 和集成提交；不删除 worktree 或分支，不为每个 worktree 单独发版。已纳入候选若验证失败或合并受阻，修复后继续，不能略过后宣称全部合并。
4. **再次 push 集成结果。** 所有候选处理完成后，运行合并后整体的适用验证；Web 改动至少执行 `pnpm test` 和 `pnpm build`，并执行 `git diff --check`。重新 fetch 并处理远端并发提交，通过正常 push 或受保护分支 PR 将最终集成内容同步到 `origin/master`。确认 `master` 与 `origin/master` 无分歧、本轮纳入的工作区无待提交代码、来源提交已包含或有补丁等价证据，才进入版本步骤。
5. If publishing a stable release after a successful prerelease or packaging-fix validation, verify the validated commit is on the release branch before versioning:
   - Use `git merge-base --is-ancestor <validated-tag-or-commit> HEAD`.
   - If it fails, do not release from `master` yet; fast-forward/merge the validated branch first, then re-check.
   - This prevents a branch-only prerelease such as a package-size experiment from being skipped by a later stable release.
6. Before packaging, prove the release `HEAD` descends from the reconciled `master` content and the latest `origin/master`. Complete the required seed-upgrade validation above when relevant, and confirm the canonical `master` worktree is clean. Seed fixes validated only from a dirty quick package must be committed on this release `HEAD` before tagging.
7. **最终统一发布一个版本。** Write a short user-visible upgrade tip covering the complete integrated features and fixes, then run the release script with the chosen version, `-UpgradeTip`, and explicit `-StageFiles` for any remaining release-code changes beyond `CMakeLists.txt` and `vcpkg.json`.
   - If the release changes packaging, include `.github\workflows\package.yml` and any release script/skill edits in the intended file list or keep them committed before publishing.
   - If `master` is protected, prepare and merge the version-file commit through its required PR before invoking the publishing script from synchronized `master`; do not tag an unmerged PR head.
   - Use `-Push` by default for an ACECode release request. Omit it only when the user explicitly requests a local-only or no-push release. Create one final stable version and tag after integration; do not publish intermediate worktree versions.
8. After the tag is pushed, stay on the task while GitHub Actions builds and publishes the complete Release. The default `-Push` path in `publish_acecode_release.ps1` waits for those assets and invokes `scripts/sync_github_release_to_aupdate.ps1`; do not interrupt or bypass that gate merely because the local Windows package already exists.
9. After the script finishes, verify `master` and `origin/master` have no divergence, `git log origin/master..master` is empty, the release tag points to the final included commit, and all expected GitHub/update-server assets came from that tag. Report the committed/pushed changes, merged and already-included worktrees, any exclusions with reasons, the final commit/tag, package paths, package sizes, SHA256 values, and verification commands. Also state that Agent Browser is integrated into Desktop and the updater ZIPs contain no legacy browser host or extension artifact.

## Stable GitHub-to-aupdate Gate (Required)

Every pushed stable release must mirror these exact GitHub Release assets before it can be called complete:

- `acecode-windows-x64.zip` -> manifest target `windows-x64`
- `acecode-windows-arm64.zip` -> manifest target `windows-arm64`
- `ACECode-<version>-macos-x64-update.zip` -> manifest target `macos-x64`
- `ACECode-<version>-macos-arm64-update.zip` -> manifest target `macos-arm64`
- `acecode-<version>-linux-x64-update.zip` -> manifest target `linux-x64-updater-v1`
- `acecode-<version>-linux-arm64-update.zip` -> manifest target `linux-arm64-updater-v1`
- `ACECode-<version>-macos-x64.pkg` -> installer download, alias `ACECode-macos-x64.pkg`
- `ACECode-<version>-macos-arm64.pkg` -> installer download, alias `ACECode-macos-arm64.pkg`

The mirror helper waits for the non-draft stable GitHub Release and all eight assets, verifies GitHub metadata plus `SHA256SUMS.txt`, inspects package structure/version/permissions, writes versioned files and latest aliases atomically, updates one six-package stable manifest record, and downloads every public versioned file and alias to recheck HTTP MIME, size, and SHA256. Either missing macOS PKG or any missing updater ZIP is a hard failure before files or `aceupdate.json` change. This is required for every full stable release even if GitHub CI permits releases without optional Installer credentials; restore those credentials and obtain both trusted PKGs before declaring the full release complete.

PKGs are separate manual installer downloads and must never be added to `aceupdate.json` package entries or replace a macOS update ZIP. Require `.pkg` to be served as `application/vnd.apple.installer+xml`. The mirror checks the flat installer XAR header and release hashes; macOS CI remains responsible for signing, notarization, and installation checks. Quick Windows validation keeps its existing Windows-only scope.

The helper preserves a semantically newer Windows-only prerelease as manifest `latest` while inserting the stable release below it. Platform selection scans compatible release records, so macOS and Linux clients can still select the new stable package.

If the GitHub Release already exists and only the mirror is missing, do not rebuild, retag, or touch the source checkout. Run:

```powershell
$upgradeTip = @'
1. Customer-facing feature or fix.
'@

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\sync_github_release_to_aupdate.ps1 `
  -Version 0.9.6 `
  -UpgradeTip $upgradeTip
```

If the helper times out or fails validation, the release is partial. Preserve the published tag/assets, fix the specific GitHub or server problem, and rerun the helper for the same version; do not create another tag just to retry mirroring.

Typical command:

```powershell
$upgradeTip = @'
1. 检查更新时可查看各版本的更新说明。
2. 优化升级流程的稳定性。
'@

powershell -NoProfile -ExecutionPolicy Bypass `
  -File .acecode\skills\acecode-release\scripts\publish_acecode_release.ps1 `
  -Version 0.2.2 `
  -Repo N:\Users\shao\acecode `
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
- For a pushed published stable release, waits for the complete GitHub Release and then runs `sync_github_release_to_aupdate.ps1`. It reports full cross-platform completion only after all six updater targets, both macOS PKGs, versioned/latest files, manifest entries, and public downloads verify.

In quick validation mode, the script:

- Auto-selects the next numeric `pre.N` version unless `-Version x.y.z-pre.N` is provided.
- Rejects stable versions, non-Windows targets, `-Push`, `-NoPublish`, `-StageFiles`, and `-CommitMessage`.
- Technically accepts a dirty working tree for rollback compatibility, but the standard speed-first workflow commits and pushes intended source changes before invoking it; the script itself never stages or commits them.
- Builds only the two Windows runtime targets and verifies `acecode.exe --version` reports the full prerelease version.
- Writes only the Windows zip plus the required `aceupdate.json` and `web.config` update-server metadata.
- Preserves existing release records while inserting a prerelease record containing only `windows-x64`.
- Verifies the Windows package and manifest over HTTP, then restores all temporarily changed source-version files.

After changing the release scripts, run both `scripts\test_quick_validation.ps1` and `scripts\test_sync_github_release_to_aupdate.ps1`. The latter must prove that a complete fixture publishes six updater targets plus both PKGs and their aliases, that PKGs stay outside the updater manifest, and that missing or invalid PKGs and a missing macOS updater fail before deployment without changing `aceupdate.json`. After any edit to this skill, run the skill creator's `quick_validate.py` against this skill folder.

## Guardrails

- Do not run `git reset --hard` or revert unrelated files.
- For stable releases, do not package from a dirty working tree unless the user knowingly accepts `-AllowDirtyBuild`. For a quick-version request, commit and push intended source changes first, then package that clean `HEAD`.
- Do not call a release complete while an included checkout has pending release code, local `master` has commits absent from `origin/master`, or an eligible completed worktree from the fixed two-day window remains unintegrated without an explicit user exclusion.
- For a full release, commit and push all pending release code first, merge the completed worktrees, push the integrated result, and only then publish one version. The release script does not discover, commit, push, or merge worktree changes for you; complete those steps before invoking it.
- Do not reuse an existing tag unless the user explicitly requests manual tag repair; the script fails on existing tags.
- Do not publish a generic package sentence as the upgrade tip; use short customer-facing feature and fix language.
- Keep `aceupdate.json` version, zip filename, SHA256, package size, and `acecode.exe --version` consistent; stable releases must also match the Git tag.
- Never call a pushed stable release complete after only the local Windows package or GitHub upload succeeds. Require all six stable updater targets in `aceupdate.json`, both macOS PKG installers, all versioned and latest-alias files in `aupdate`, and successful public body hash verification. A DMG, PKG, or `.tar.gz` does not substitute for a macOS `*-update.zip`.
- Never turn quick validation into a partial release: do not pass `-Push` to the quick-validation script, create a tag, publish GitHub assets, or mirror non-Windows packages. Pushing the ordinary source commit from step 1 is separate and required.
- Remember that publishing a prerelease to the shared Jenkins Green manifest makes it visible to clients using that update service. Use the next `pre.N`, not an existing version, when testers must receive another update.
- Confirm the source checkout is clean after the step-1 commit. If the quick-validation script still reports dirty paths, stop and exclude secrets, generated output, private machine artifacts, or unrelated work before packaging.
- Do not intentionally publish a quick package from dirty source files. Commit and push the exact intended changes first so the packaged quick version is traceable to `HEAD`.
- Remember that users upgrading from older binaries see the old updater UI during the first upgrade; new UI changes are visible after the upgraded binary is installed.
- Never infer that existing users receive a new seeded Skill, expert, or hook merely because it is present under `share\acecode\seed` in the package. Prove the old-marker and equal-marker upgrade paths with the final packaged executable, and state the required restart behavior in the handoff.
- Treat successful tagged or committed prerelease points as validation evidence, not as automatically released code. Before a stable release, prove the validated prerelease or package-fix commit is an ancestor of the stable release commit.
- Keep package-size protections in source control, preferably as CI checks, so they fail before release rather than relying on memory:
  - `.github/workflows/package.yml` package builds must use `MinSizeRel`, not `Release`.
  - Linux packages must strip runtime binaries with `strip --strip-unneeded`, not only `--strip-debug`.
  - macOS packages must strip the desktop app executable and bundled `acecode-daemon` after creating dSYM artifacts.
  - `acecode-desktop` must not link the broad `acecode_testable` object set or FTXUI; use focused desktop/native support targets so the desktop shell does not pull agent/TUI/web/server/provider code.
  - CI should enforce package size budgets for release assets and fail on regressions. Calibrate budgets from the last known-good release assets, then update intentionally with evidence when size growth is expected.
- Keep Unix `.tar.gz` archives and latest aliases as downloadable assets only; never put a `.tar.gz` URL in `aceupdate.json`, because the self-updater consumes ZIP packages.
- Before publishing Linux self-update entries, read `docs/linux-self-update.md` from the release commit and prove that the release contains the `manifest_target_for_platform` capability mapping. Publish only verified `acecode-<version>-linux-{x64,arm64}-update.zip` assets under manifest targets `linux-{x64,arm64}-updater-v1`.
- Never publish a Linux updater ZIP under legacy manifest targets `linux-x64` or `linux-arm64`. ACECode versions through v0.8.8 can extract those files without restoring executable permissions, so matching them to a ZIP can leave `acecode` non-executable. Those clients require one integrity-verified manual bootstrap to a release that supports the versioned target.
- For each Linux updater ZIP, verify the exact version, SHA256, byte size, one top-level directory, executable modes for `acecode` and `acecode-desktop`, the required `models.dev` files, and absence of retired `ace-browser-*` artifacts before mirroring it and updating the manifest. After publication, fetch the manifest and every Linux ZIP through the public update URL and repeat the hash, size, ZIP-layout, permission, and version checks.
- macOS updater entries remain governed by their existing tested ZIP/AppBundle path; do not infer Linux support from macOS packaging or vice versa.
