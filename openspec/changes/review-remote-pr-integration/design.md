<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

Sidebar navigation now projects disk history before runtime recovery finishes. Pending and failed recovery must remain distinct from a ready runtime. QuestionPicker now sits inside the composer dock, whose existing non-shrinking block layout does not constrain a tall options list.

## Decisions

- Preserve the PRs' histories and repair them on the current branch before publishing the integration.
- Mark a failed resume explicitly and suppress live monitoring until a subsequent successful resume. Allow disk history to remain visible; restrict the composer during recovery.
- Treat transcript identity as session ID plus API endpoint. Promotion to live monitoring refreshes/catches up without clearing already displayed history.
- Use the existing chat flex layout to constrain questions. Header and footer remain outside the scrolling options region.
- Accept the daemon launcher's documented already-running exit code separately from launch failures. Never infer success from an arbitrary port file.
- Keep unrelated concurrent edits outside the integration commits.

## Validation

Run focused behavioral regressions, frontend tests/build, launcher tests, configurable-Ask C++ tests, strict OpenSpec validation and whitespace checks. Use a browser fixture for constrained question heights. Record any native/runtime coverage limitations.
