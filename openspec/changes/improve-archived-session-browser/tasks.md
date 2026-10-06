## 1. Archive browsing logic

- [x] 1.1 Add workspace grouping, combined search/workspace filtering, update-time sorting, and selection pruning helpers; verify unit coverage for duplicate names/IDs, no-workspace/local rows, invalid dates, empty results, and immutable input.

## 2. Archive interface

- [x] 2.1 Add the header batch controls, search/sort/workspace toolbar, and grouped counted rows; wire selection to visible results and retain single/batch operation behavior, verified with focused checks and browser interactions.
- [x] 2.2 Add English overrides and regenerate the source catalog; verify localization audit and review generated changes.

## 3. Integration verification

- [x] 3.1 Run the complete web tests and production build, validate the OpenSpec change strictly, and inspect the final diff while preserving unrelated edits.
- [x] 3.2 Verify desktop, narrow, and dark archive layouts and combined filters, visible-only selection, restore, delete confirmation/cancellation, and partial-failure retry against isolated mock sessions; capture a screenshot.

Verification: `pnpm test` passed 2,174 cases; `pnpm build` and the regex compatibility gate passed; localization audit and strict OpenSpec validation passed. Headless Chromium exercised nine isolated mock sessions across three workspaces and no-workspace tasks, including ten restore/delete requests, injected partial failures, cancellation, scoped selection, locale switching, and 1600/760/430-pixel viewports with no page errors or horizontal content overflow. Screenshots were captured outside the repository. Browser QA also confirmed the scoped archive confirmation layer fix.

## 4. 2026-10-05 工作区顺序修正

- [x] 4.1 分组助手支持侧边栏工作区顺序，时间排序仅作用于组内；覆盖正反序、搜索、同名工作区、特殊分组及删除后的顺序稳定性。
- [x] 4.2 归档页面复用接口返回的工作区顺序，分组和筛选选项保持一致。
- [x] 4.3 执行定向回归、完整 Web 测试与生产构建、OpenSpec 严格验证和差异检查，并验证实际页面的排序交互。

本次验证：归档助手 15 项、批量操作结构 3 项定向检查通过；`pnpm test` 全部通过（日志包含 3,024 条 `[pass]`）；`pnpm build` 与正则兼容性检查、`openspec validate improve-archived-session-browser --strict`、`git diff --check` 通过。Chromium 使用隔离 API 数据验证工作区分组与筛选顺序、组内正反时间排序、选择保留、组合筛选、取消归档后分组顺序以及修改工作区顺序后重新打开页面；1440px 和 430px 页面无横向溢出、无页面异常。没有重新构建或启动 Desktop。
