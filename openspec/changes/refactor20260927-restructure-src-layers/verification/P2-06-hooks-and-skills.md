# P2-06 hooks 与 skills 拆分验证

任务认领由 PR #82 合入 master,基线 `d993ae5f00c2d4c475a04c05d5509ffbbe3b4d8e`,已包含 P2-03 / P2-04 / P2-05 的验收。工作分支为 `refactor20260927/P2-06`,复用已合入且无活动进程、无未提交改动的原 P2-03 工作区;构建使用新目录。

## 实现边界

- `51c37dea`:13 个文件的纯 `git mv`,逐项 R100,0 行增删。frontmatter 归 utils,opencode_command 与 skill_command_expander 归 skills,skill_commands 归 tui/commands;五个测试同步镜像归位。
- `f095f762`:19 个源/测试文件的 20 处 include 路径改写;CMake 删除失效的根级 skill_commands.cpp 子集项,已登记的 commands/ 子集继续确保该实现参与 acecode_testable。
- 模型注册源、模型加载完成和助手消息完成三个事件构造器及其私有辅助移到 agent/hook_bridge/hook_events。hooks/hook_payload 仅保留无 provider 依赖的启动前事件;调用方直接引用实际定义头,旧路径无转发壳。
- DefaultHookSeed 与只读种子表移到 hooks/hook_seeds。注册表只依赖 hooks;skills 安装器继续协调完整种子事务并读取该表,既有版本/哈希/迁移规则保持。
- 两类启动事件复用已有 platform 进程 ID 原语,保留原来的 int 转换。移除终端探测对 hooks 配置的无用 include。
- 同步 CLAUDE.md、技能说明和对应测试注释中的路径;help-source 生成器未引用此次移动路径,无须改写生成内容。

## 当前检查

src / tests include 规范化第二次检查为 0 改动;映射 strict、分层已有阻断项、行数和所有权 strict 通过。分层违规从 P2-05 的 87 项降为 83 项,本任务相关 hooks→provider、hooks→skills 和 environment→hooks 反向依赖消除。其余项目仍由后续 P2-07 / P2-08 处理。

前端首次测试因本工作区未安装 node_modules,缺少 @babel/core 而未能启动完整套件;已执行 pnpm install --frozen-lockfile,不改锁文件。依赖齐备后的完整 `pnpm test` 与 `pnpm build` 均通过。

对照机械提交前后的函数文本,三个模型事件构造器除了等价的进程 ID 原语调用外保持原样;hook 种子表函数完全相同。

原生构建、目标逐元组对照、完整用例/SKIP 清单与四平台 CI 尚待执行。任务保持未勾选,通过并合入后再更新。
