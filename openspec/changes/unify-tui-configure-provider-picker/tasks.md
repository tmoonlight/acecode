## 1. 统一 Provider 候选模型

- [x] 1.1 在 `configure_catalog` 中增加带类型的统一候选结构和构造函数，固定两个自定义接口、Copilot、models.dev 预置的顺序，并消除普通 `github-copilot` 重复项。
- [x] 1.2 增加当前配置到默认候选索引的纯函数，覆盖自定义 OpenAI、Anthropic、Copilot、目录 Provider 与未配置回落。
- [x] 1.3 拆分 catalog Provider 的选择与应用逻辑，使已选目录项能直接进入 Base URL、密钥和模型配置。

## 2. Provider autocomplete picker

- [x] 2.1 扩展 `PickerOptions` 与纯过滤 helper，支持可选的直接输入搜索模式和搜索提示文本。
- [x] 2.2 在 FTXUI 路径渲染搜索文本框，实现直接键入、退格、首条建议高亮、方向/翻页导航、回车确认以及 Esc 清空/取消。
- [x] 2.3 在非 TTY stdin 回落路径中支持直接输入查询文本，同时保留编号、翻页、`/<query>` 和取消兼容行为。

## 3. configure 路由与回归测试

- [x] 3.1 用统一 picker 替换 `run_configure()` 的四项顶层菜单，并按候选类型路由到现有 OpenAI、Anthropic、Copilot 或 catalog 配置流程。
- [x] 3.2 扩展 picker 与 catalog 单元测试，覆盖搜索字段匹配、候选顺序、重复项消除和所有默认高亮分支。
- [x] 3.3 修正统一流程下 Anthropic 的 Source 摘要，并确认已有 catalog/Copilot 保存语义不变。

## 4. 验证

- [x] 4.1 构建 `acecode_unit_tests` 并运行 `ConfigureCatalog`、`ConfigurePicker` 相关定向测试。
- [x] 4.2 运行 `git diff --check`、`scripts/code_quality_check.bat` 和 `openspec validate unify-tui-configure-provider-picker --strict`。
  - `git diff --check` 与严格 OpenSpec 校验通过。Windows 批处理脚本因既有的 `EQU`/中文输出解析错误提前退出；同一仓库的 `scripts/code_quality_check.sh` 等价检查通过。
- [x] 4.3 手动检查统一列表在 catalog 可用/不可用时的候选集合、搜索提示和取消语义，并记录无法自动覆盖的交互验证边界。
  - 在真实 TTY 中确认最新的 168 个预置加 2 个自定义入口、ACEModel 第三位、直接输入 `router` 的过滤结果、方向键高亮，以及第一次 Esc 清空、第二次 Esc 取消且不保存。catalog 不可用的四项退化集合由空目录候选纯函数测试覆盖；未修改真实配置来破坏内置快照，因此该缺失快照场景未做二进制端到端模拟。

## 5. 同步 ACEModel 预置

- [x] 5.1 抽取 ACEModel 的 Provider ID、名称、固定 Base URL、API Key 环境变量和内置模型为 Web/TUI 共用的共享定义。
- [x] 5.2 在统一 TUI 列表第三位插入共享 ACEModel 预置，把 Copilot 顺延到第四位，并按 ID 排除内置预置与 models.dev 的重复项。
- [x] 5.3 让 Web catalog 摘要与模型查询改用共享 ACEModel 定义，并补充候选顺序、默认高亮、元数据与去重测试。
- [x] 5.4 构建并运行相关定向测试，完成严格 OpenSpec、diff 与真实 TTY 搜索/选择验证。
  - 隔离 QA 输出完成 `acecode` 与 `acecode_unit_tests` 链接；21 个相关测试全部通过。真实 TTY 输入 `acemodel` 得到唯一候选，回车后进入共享预置流程并显示固定默认端点与 `ACEMODEL_API_KEY`，在保存前终止。严格 OpenSpec、`git diff --check` 与质量脚本通过。
