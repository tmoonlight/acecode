<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md. The normal background-exit policy currently also applies to upgrade restart. Managed-daemon discovery verifies ownership but accepts any application version with protocol 1. The updater applies files in the serving daemon's executable directory, whereas Desktop relaunches its own captured install path. Windows/Linux application verification only checks file existence.

## Goals / Non-Goals

**Goals:** Keep normal daemon continuation while making upgrade handoff and installation version-aware. Recover existing old daemons on the first launch of the fixed Desktop. Keep all destructive replacement behind existing managed identity verification.

**Non-Goals:** Stop standalone CLI daemons, change background task persistence preferences, introduce a new updater process or protocol number, or modify user configuration during version probing.

## Decisions

1. Give pool shutdown an explicit upgrade-restart reason. That reason always stops supervisors. Supervisors wait for process exit and surface failure; Desktop must not launch a replacement after an unsuccessful teardown. Normal shutdown still honors the saved preference.
2. Compare the verified managed daemon's live application version with the Desktop build version and its actual process executable path with the daemon located beside Desktop. Extract process path lookup from the existing identity helper and expose a testable compatibility decision. An unreadable path fails safely; an owned but different installation/version is replaced. PID/GUID/protocol ownership checks remain prerequisites.
3. Probe the staged executable using a direct, bounded `--version` child process after archive checksum/signature checks. Parse only the ACECode version output and require it to match the selected manifest release. Flat-package application accepts an expected version and repeats the probe before committing success, rolling back on mismatch. macOS app updates retain their signature/bundle checks and additionally verify the daemon binary before installation. Do not use a shell or execute Desktop GUI to obtain a version.
4. Return the existing completed GUI job when a successful installation still requires restart. The old daemon remains the owner of that job until it stops; no persistent global marker is necessary. Failed/cancelled jobs remain retryable.
5. Existing `manage-desktop-background-process` design allows differing application versions and applies continuation to upgrade restart. This change supersedes those two choices for upgrade correctness; normal compatible reuse and exit behavior are retained.

## Risks / Trade-offs

- An explicit upgrade restart ends managed background work, as the original restart requirement specifies; it does not change the saved continuation preference.
- Multiple installation copies share a runtime directory. Ownership checks must precede replacement, and actual executable paths must be compared using filesystem identity/canonical paths.
- Broken version probes must fail with a timeout/output limit and clean up only their own child process. Legacy releases still print `acecode v<version>` and are compatible with the probe.
- A user upgrading from an old buggy Desktop may keep its old daemon on that first restart. The new Desktop's version/path check provides the recovery path.

## Migration Plan

Run focused native and API regression tests, Web checks, and package verification. Publish the next unused stable version from reconciled master, await all GitHub platform assets, then mirror and publicly verify all six updater targets plus the requested downloadable artifacts. Preserve the previous release and package backups for rollback.
