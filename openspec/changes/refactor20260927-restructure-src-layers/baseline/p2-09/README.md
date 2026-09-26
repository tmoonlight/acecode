# P2-09 分支迁移工具验收

本次只实现并验证迁移工具。没有合入九个遗留功能分支，也没有执行真实 P2/P3 搬迁、修改原 refs/worktrees、推送或标勾任务。

- 固定生产基线：`7621b4f6fac524740296147aed4eaeeacd0cdd8a`。
- 最终工具验证提交：`2cd7bc6494c1b40ecfde8cc2e876cdb2342ea0a8`，已合入上述 master。
- 映射表 SHA-256：`379dab9f88d102df860934c4d635d990285a88906196e3b5ce53fef804e11c78`。
- 九个遗留 HEAD 使用 [P0-02 原盘点](../../branch-inventory.md) 的固定 SHA。最终 27 次操作及源 refs/index/working diff 完整性核对见 [final/manifest.json](final/manifest.json)。每次报告均保留 merge-base、目标/来源 SHA、Git 原始 stdout/stderr、冲突路径、patch SHA、对象 bundle 和隔离仓库路径。

## 五模式与工具测试

`rebase`、`patch` 只在新建隔离仓库工作；`--apply-map` 做 tracked-only 路径/include/CMake 计划；`--docs` 分开处理 authored 文档/help 生成和 seed 版本事务；`--check` 聚合正式阻断检查。具体命令与边界见 [工具 README](../../../../../scripts/refactor/README.md#legacy-branch-migration-p2-09)。

最终工具集 **50 项测试通过**（65.687 秒）：P0-03 及主代理 snapshot 修复 31 项，迁移专项 19 项。迁移专项覆盖真实 Git rebase、真实 `git apply --3way --index`、导出 bundle 后在另一仓库三方应用、中文文件名/二进制/混合行尾/无末尾换行、上下文 CMake 和相对文档路径、最长前缀与大小写碰撞、真实嵌套 worktree、未跟踪文件保护、冲突保留、删除/语义提取拒绝静默处理、help 失败全事务不写入、seed 版本/hash/测试联动及最终检查正反例。

```text
python -m unittest discover -s scripts/refactor/tests -p test_*.py -v
python scripts/refactor/tests/rehearse_legacy_refs.py --base 7621b4f6fac524740296147aed4eaeeacd0cdd8a --jobs 3 --output-dir REPORT_DIRECTORY
```

## 九 ref 的真实结果

每个 ref 均执行：真实旧布局 patch、真实旧布局 rebase、显式标记的目录/include 投影 patch。当前固定 master 尚未完成 P2/P3，因此投影只是安全 fixture：不执行语义提取、不擅自删除计划中的文件、不宣称可编译，不作为冻结基线。

所有真实旧布局 patch/rebase 都遇到旧分支与当前代码的历史冲突；七个投影 patch 实际调用 Git 后也有冲突，另两个因为改动涉及计划删除文件而在生成 patch 前明确阻断。不存在被宣称成功的遗留分支。工具正向成功路径由独立的真实 Git 集成 fixture 覆盖。

| 遗留 ref（省略 `origin/`） | 当前 patch | 当前 rebase 冲突文件数 | 投影 patch | 具体原因摘要 |
| --- | --- | ---: | --- | --- |
| chatview_optimize | Git 1 | 5 | Git 1 | 历史根目录 `main.cpp`、ask_overlay_input 头已不存在；CMake 与 Web 内容冲突。[报告](final/01-current-patch.json) |
| claude/ai-image-sharing-tool-8ewihy | Git 1 | 4 | Git 1 | 历史 `main.cpp` 已不存在；builtin registry、show_image 与 tui_state 冲突。[报告](final/02-current-patch.json) |
| claude/debug-acecode-crash-XNWgr | Git 1 | 9 | Git 1 | desktop main/web_host、web server/smoke 与前端菜单/Sidebar/API 冲突。[报告](final/03-current-patch.json) |
| claude/desktop-skill-error-handling-r3j2h8 | Git 1 | 3 | Git 1 | C++ patch 可应用，整个分支仍因 SettingsPage 与两个 i18n 文件冲突而失败。[报告](final/04-current-patch.json) |
| claude/fix-desktop-context-compression-tkOxK | Git 1 | 2 | Git 1 | AgentLoop cpp/hpp 内容冲突。[报告](final/05-current-patch.json) |
| claude/multi-model-config-design-wWhDX | Git 1 | 3 | Git 1 | 历史 `main.cpp` 已不存在；builtin_commands 与 slash_dropdown 冲突。[报告](final/06-current-patch.json) |
| codex/add-self-session-control | Git 1 | 7 | 明确阻断 | pinned_sessions_handler.cpp 在映射中计划删除；tool_executor.hpp 还涉及 ToolResult 提取，不能丢弃或复活旧改动。[报告](final/07-projection-patch.json) |
| docs/askuserquestion-dual-entry-design | Git 1 | 2 | 明确阻断 | 当前 patch 命中已删除的 overlay 文件及业务冲突；投影另因 tui_init.cpp 计划删除而阻断，并登记 ToolResult 提取。[报告](final/08-projection-patch.json) |
| jb | Git 1 | 7 | Git 1 | AgentLoop、system_prompt、session_registry、settingsSearch 冲突；另登记 config.cpp 路径函数提取。[报告](final/09-projection-patch.json) |

Git 在发现不存在于 index 的旧文件时可能整体拒绝 patch，虽然 stderr 已列出内容冲突，但不会留下 unmerged entries。因此 `conflicts=[]` 不能证明成功；工具始终使用真实 Git 返回码和完整诊断判定。

历史根目录 `main.cpp` 和早已移除的 overlay 文件不属于本次正式 src 映射的整文件改名；工具如实报告，未凭文件相似度猜测语义迁移。P3-03 应在真实搬迁后重跑并逐分支人工移植/裁决，不应把这些功能提前混入结构重构。

## 真实目录、文档与 seed 演练

- [current-map-plan.json](current-map-plan.json)：旧布局 dry-run 计划 1061 个整文件移动、636 个内容更新文件；报告 20 个计划删除、17 个语义提取、15 个指向计划删除文件的 include、3 个歧义 include。未写生产文件，也未把这 55 项问题豁免为已完成。
- [projection-doc-plan.json](projection-doc-plan.json)：在固定基线的明确目录投影上，从 tracked 快照运行原 `build_help.py` 成功，生成 49 篇文章、205 条搜索记录、44 张已拍图与 10 个占位。计划更新 195 个文档；4 个生成物由 builder 生成，没有直接正则编辑 `sources.json` 或 `search-index.js`。这是 dry-run，原 repo 与投影 repo 文档均未写入。
- [current-seed-plan.json](current-seed-plan.json)：dry-run 将 6 处路径归入两个 SKILL，单独计划 `seed.version`、MANIFEST 版本及两个 canonical-LF hash。没有实际 bump；当前真实 C++ 测试动态读取 seed.version，旧包版本硬编码命中 0，历史升级 fixture 版本保持不动。
- [current-check.json](current-check.json)：最终模式在真实旧布局上正确退出 1，共 **4823** 项：R1–R14 3515、include 规范化 1168、文档失效路径 80、待迁移 build/authored-doc 路径文件 60。映射、R15 棘轮、seed 一致性均为 0。正式分层工具在旧布局上还会报告旧根/模块归属，不能与 P0 transition 模式 1326 项直接混为一个基线。

实际 P2/P3 尚未完成，故未宣称真实遗留分支 `--check` 为零，也未执行四平台构建/target 快照验收。完整正向静态 gate 和反向失败路径已由 fixture 验证；真实 P3-03 合入前仍需冲突/提取审查、零静态 finding 和实际构建测试。
