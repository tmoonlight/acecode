# Proposal: refactor20260927-restructure-src-layers

> 本变更是 **refactor20260927 系列**的母变更,同时承载整体路线图、决策登记和协作约定(见 design.md)。系列共 4 个 change,名称统一带 `refactor20260927-` 前缀:
>
> | change | 内容 | 阶段 |
> |---|---|---|
> | `refactor20260927-restructure-src-layers`(本变更) | src/ 分层与目录搬迁、护栏与工具、include 规范化、文档 | P0–P4 |
> | `refactor20260927-split-agent-loop` | 拆分 `agent_loop.hpp/.cpp`,构造注入 | P6A |
> | `refactor20260927-split-tui-main` | 拆分 `src/main.cpp`,TUI 收成 `TuiApp` | P6B |
> | `refactor20260927-adopt-ownership-conventions` | 所有权与生命周期整改,含 D6–D9 四项行为变更 | P7-O |

## Why

`src/` 下 40 个目录平铺在同一级,维度和颗粒度混在一起:`utils` 是基础库,`session` 同时管持久化、多会话宿主,还夹着一个直接调模型的 `side_chat`;`desktop` 既是桌面壳,又放着 daemon 和工具都要用的工作区注册表;`connectors` 只有 2 个文件却和 87 个文件的 `session` 同级。根目录还散着 `agent_loop.cpp`(6952 行)、`main.cpp`(8827 行)、`permissions.hpp`、`tui_state.hpp`。依赖方向也是乱的:实测 `utils → config/provider/tool`、`provider ↔ session`、`session → tui_state/web/desktop`、`skills → agent_loop.hpp` 等反向边共 100 多条,读代码的人看不出层次,新代码也不知道该放哪。

前期调研(约 30 个代理的只读盘点、三套分层方案比选与对抗评审)已经完成。实测并行分支的冲突面很小:只有 9 个 ref 带独有的 src 改动,每个最多 16 个文件。现在动手的成本最低。

## What Changes

- **6 个分组,组名即依赖方向**:`src/` 重排为 `base < domain < adapters < engine < host < apps`,只允许上层 include 下层。现有模块大多整体改名挂到组下,模块名不变,例如 `src/session` → `src/domain/session`、`src/tool` → `src/adapters/tool`。
- **6 个 include 根**:6 个分组目录各自作为 include 根,include 一律写成 `"<模块>/…"`。冻结期整目录改名时,include 字符串一个字不改。
- **分层唯一事实源与机器检查**:新增 `src/layers.tsv`(路径前缀 → 模块 → 分组 → rank → 禁止规则 → 例外),配套 `scripts/layers/`、`scripts/refactor/` 下的分层 lint、行数棘轮(新文件不超过 1000 行,存量只减不增)、所有权棘轮。先以报告模式接入 CI,搬迁完成后转为阻断。
- **CMake 护栏**:TUI 源的切分从正则 `/src/(tui|markdown)/` 改为目录变量,并在集合为空时 FATAL。所有显式源清单、按源设置的属性、`.mm`、`REMOVE_ITEM` 都加存在性断言。搬迁后改为 6 根 include。
- **冻结前切断反向依赖**:约 12–15 个小 PR,在冻结前完成文件换模块与拆头。最关键的一刀是把 `provider/llm_provider.hpp` 下沉到 `domain/llm` 并删掉它第 3 行对 `retry_policy.hpp` 的 include,一次消掉 provider↔session、provider↔tool、provider↔pa 三个环。
- **删除已核实的死代码**,约 1950 行:TUI 半抽取副本(`tui/cli_dispatch`、`tui_init`、`tui_context`、`agent_callbacks_builder`、`terminal_utils`、`clipboard_helpers`、`ime_windows` 等)、main.cpp 的 IME 死代码、`daemon/supervisor.*` 等。
- **冻结半天,只做纯改名**:全部提交都是 R100 的 `git mv`,随后机械修正 CMake、文档和测试路径;`tests/` 按模块名镜像。
- **文档**:新增 `docs/architecture/src-layout.md`(层定义 + 「新文件放哪」决策表),更新 ARCHITECTURE.md、AGENTS.md、AGENT.md、CLAUDE.md。
- **行为**:本变更**不改变任何用户可见行为**。系列里的全部行为变更集中在 `refactor20260927-adopt-ownership-conventions`。

## Capabilities

### New Capabilities
- 无。本变更是纯结构重构,外加工具与文档,不引入也不修改任何 spec 级行为;`.openspec.yaml` 已设 `skip_specs: true`。

### Modified Capabilities
- 无。

## Non-goals

- `web/` 前端(React)不动。`src/web` 只随搬迁改 include 前缀,少数被下层借用的文件移出(见 design D21),内部不重构。
- agent_loop 与 main.cpp 之外的超 1000 行文件本期不拆,放二期 P6C;在此之前由行数棘轮保证只减不增。
- 不按组建 STATIC 库,放二期 P5;本期由文本 lint 保证分层。
- 不做三入口共用的组合根与 ConfigStore,放二期。
- 不升级 C++ 标准,不引入 C++20 模块。Deepin/UOS 包的构建容器是 GCC 8.3,这是硬门槛。

## Impact

- **代码**:src/ 下约 975 个文件的路径变化;tests/ 约 500 个文件随模块镜像;约 1124 行相对 include 改写;CMakeLists.txt、tests/CMakeLists.txt、cmake/*.cmake。
- **文档**:
  - CLAUDE.md 约 97 处路径;ARCHITECTURE.md、AGENTS.md、AGENT.md;
  - docs/ 已跟踪文件约 578 处,help 站点改源头 `docs/help-source/group*.py` 后重新生成;
  - seed SKILL.md 6 处,需要同步 bump seed 版本;
  - 9 个前端架构测试读取的 C++ 路径收敛为一张路径表,只改测试(D4)。
- **协作**:冻结窗口半天;9 个带独有 src 改动的分支用 `migrate_branch.py` 迁移;机械提交记入 `.git-blame-ignore-revs`。
- **构建与 CI**:Windows、macOS、Linux、arm、Deepin 都要在全新构建目录验证;CI 新增 layer-lint job,以及只能手动触发的 refactor-matrix(Windows/macOS 单测)。
