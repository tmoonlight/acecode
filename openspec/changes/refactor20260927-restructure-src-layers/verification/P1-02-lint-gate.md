# P1-02 lint 阻断 `../` 与 .git-blame-ignore-revs 验证记录

认领 Claude-phase1,2026-09-27(`5dafcea7`)。分支 `refactor20260927/P1-02`:

| 提交 | 内容 |
|---|---|
| `2a65e8a3` | test.yml `layer-lint` 作业:`normalize_includes.py --check` 的 src / tests 两个范围去掉 `\|\| notice` 兜底改为阻断;`.git-blame-ignore-revs` 登记 P1-01 的两个 [mechanical] 提交(`5524a352`、`7a90db6d`);AGENTS.md「Commit & Pull Request Guidelines」写明 `git config blame.ignoreRevsFile` 用法 |
| `9e5453a6` | 补上 `check_layers.py --enforce-parent-includes`(前一提交的替换只命中了注释行,命令行漏改;本地跑 CI 同款命令时用的是手写命令,没能暴露,靠复核 `grep -n check_layers.py` 发现) |

作业名保持 `layer-lint (report mode)` 不变(分层 R1–R14 的其它子项与 doc-paths 仍是报告模式;阻断项现在是 `../` include、include 规范化、行数 / 所有权棘轮、映射校验、工具自测)。

## 验证

| 项目 | 结论 | 证据 |
|---|---|---|
| 本地 CI 同款命令(`bash -eo pipefail` 逐条) | 通过 | check_layers `--enforce-parent-includes`、check_doc_paths、normalize_includes `--check` src / tests、check_file_size / check_ownership / validate_map `--strict` 全部 exit 0 |
| `git blame` 跳过机械提交 | 通过 | `git blame --ignore-revs-file .git-blame-ignore-revs -L 2,3 src/channels/bridge.cpp` 两行 include 归到 `cc5956e4`(2026-09-17),不加参数时归到 `5524a352` |
| CI 正向:`test.yml` 在分支头 `9e5453a6` 上手动触发 | 通过 | run 36308301151 |
| CI 反向:故意新增一行 `../` include,layer-lint 作业失败 | 失败(符合预期) | artifact `layer-lint-reports` 的 layers.json 含 1 项 `R8 parent-relative include is forbidden`(`src/channels/bridge.cpp:4`,target `../utils/utf8_path.hpp`);作业在「Layer, size, ownership, doc-path and map checks」步骤 exit 1,`Refactor tool self-tests` 未执行。一次性分支 `refactor20260927/P1-02-negative`(在 `9e5453a6` 之上给 `src/channels/bridge.cpp` 加一行 `#include "../utils/utf8_path.hpp"`),run 36308298784;验证后取消该 run 的其余作业并删除分支 |
| 合入后 master 的 push 检查 | 见 P1 阶段汇总 | P1-01 合入触发 run 36308130247(layer-lint 已通过,unit-tests 进行中);P1-02 合入触发的 run 一并记入 `P1-acceptance.md` |
