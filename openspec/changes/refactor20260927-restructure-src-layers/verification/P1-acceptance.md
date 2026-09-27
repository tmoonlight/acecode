# Phase 1 验收记录(design.md §7.2「P1」行)

验收人 Claude-phase1,2026-09-27。P1 只有两个任务,均已合入 master 并推送:P1-01(`f9911d89` 合入 `refactor20260927/P1-01`)、P1-02(`c9ede6cd` 合入 `refactor20260927/P1-02`)。逐项证据见 `P1-01-include-normalization.md` 与 `P1-02-lint-gate.md`,本记录只对照 §7.2 给结论并补记合入后的 master 检查。

## 闸门逐项

| §7.2 P1 条目 | 结论 | 证据 |
|---|---|---|
| numstat 满足每个文件「增加 = 删除 = 改动的 include 行数」 | 满足 | src 369 文件 / 1150 行、tests 48 文件 / 54 行,histogram numstat 逐文件相等,补丁正文只有 include 行(P1-01 记录第 1 节) |
| 第二次运行 0 diff | 满足 | `normalize_includes.py --check` src / tests 均 0 diff、0 errors;P1-02 起在 `layer-lint` 作业里阻断 |
| src 下不再有 `../` | 满足 | grep 为 0;`check_layers.py --enforce-parent-includes` exit 0;P1-02 反向验证:故意加一行 `../` include 的一次性分支,layer-lint 作业失败(run 36308298784) |
| Win/Linux/mac/arm/Deepin 全新目录构建通过 | 满足 | `package.yml` run 36304753531 全部 11 个平台作业成功;`refactor-matrix` run 36305642345(Windows / Linux / macOS)+ run 36304751667(Deepin);本机 Windows 全新目录 `build-p1` |
| 用例清单与 SKIP 清单等于 G0(D23:post-p0) | 满足 | 四平台 `tests` / `ctest_names` / SKIP 集合与 post-p0 逐条相同;失败集合的差异只有两条已知抖动(`AgentLoopTurnSteering.EveryAcceptedFinalBoundaryRaceInputIsCommitted` Windows 一次、`OpenAiProviderErrorRecovery.SseKeepaliveCommentsDoNotTriggerRetry` macOS) |

## 合入后 master 的 push 检查(test.yml)

| 提交 | run | 结论 |
|---|---|---|
| `f9911d89`(P1-01 合入) | 36308130247 | 通过 |
| `c9ede6cd`(P1-02 合入) | 36308407289 | 通过(layer-lint 已带 `--enforce-parent-includes` 与阻断的 include 规范化) |
| `fdb91d1f` / `d40762c7` / `4b6ce35c` / `bf1ef7db`(P2 认领与工具提交) | 36308439936 / 36308537424 / 36308797819 / 36310062586 | 通过 |
| `187bdd50` | 36308731435 | **失败**:工具自测文件的换行转义被 heredoc 塌掉导致 Python 语法错误,立即由 `4b6ce35c` 修正;记录在案,不影响 P1 结论 |

## 遗留

- 9 个遗留 ref 的处理方式已在 `branch-inventory.md`「P1-01 公告」写明(先在分支上跑 `normalize_includes` 再 rebase;走 patch 迁移路线的由 `migrate_branch --apply-map` 处理)。
- P1 之后本机(Windows)全量单测基线:5115 列出 / 5114 执行 / 9 SKIP / 0 失败,P2 各 PR 以它与 `local-win-p1` 的目标快照为本机对照(CI 仍以 `baseline/g0/post-p0` 为对照)。
