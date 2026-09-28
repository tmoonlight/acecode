<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See `proposal.md` for motivation and scope. The starting branch is clean `master` at `87ff29ea`. PR #65's net merge diff touches 19 paths. Subsequent work overlaps only `web/src/styles/globals.css` and `web/src/lib/runTests.js` within those paths, adding independent queue styles, a composer font adjustment, and test registrations.

## Goals / Non-Goals

**Goals:** Restore the exact pre-PR #65 implementation wherever no later changes overlap; preserve later changes in shared files; keep restored copy localized.

**Non-Goals:** Redesign the sidebar, revert other contributions by the same author, rewrite history, change branches, publish packages, or alter daemon behavior.

## Decisions

- Apply `git revert --no-commit -m 1 398f906c3b77d31d7d2967e4c3bb122b9a358c26`. Reverting the net merge includes the onboarding integration adjustments and avoids undoing unrelated commits brought into the PR branch. Resetting to the old parent would discard subsequent work; restoring complete shared files would lose unrelated edits.
- Keep the original regression tests restored by the revert and regenerate the localization catalog. English overrides are added only if the restored strings require them. The existing `webui-custom-sidebar` specification does not cover these visual details; this change adds the restored behavior without rewriting unrelated historical requirements.
- Compare the zero-context rollback diff with the original merge's inverse, ignoring only blob IDs and hunk offsets, and separately compare preserved later changes in shared files. Run the existing frontend suite, production build, motion checker when its browser dependency is available, and strict OpenSpec validation.

## Risks / Trade-offs

- Shared-file overlap could erase later work -> use a three-way revert and compare the two shared-file deltas against the pre-revert HEAD.
- Restored onboarding text may be missing from the current source catalog -> regenerate and review the localization diff before tests.
- Web checks do not validate an installed desktop executable -> report the validation scope explicitly; package deployment remains a separate action.

## Migration Plan

Create a normal local revert commit after validation. The rollback needs no data migration; it can itself be reverted to reapply the PR if later requested.
