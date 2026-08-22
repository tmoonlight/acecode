## 背景

`run_configure()` 当前用 `read_choice()` 展示四个顶层入口，其中 catalog 入口再调用 `run_provider_picker()`。后者虽然已有 FTXUI picker，但交互仍是命令式过滤：用户必须先按 `/` 才能输入查询。`configure_openai_via_catalog()` 同时承担“选择 Provider”和“应用已选 Provider”两项职责，导致顶层入口无法直接与目录项组成同一个列表。

现有 `configure_openai()`、`configure_anthropic()`、`configure_copilot()` 以及 catalog Provider 选定后的 Base URL、密钥和模型配置逻辑都已可用。本次只重组选择层和 picker 输入方式，不改变这些流程的认证与持久化语义。

## 目标 / 非目标

**目标：**

- 以一个 FTXUI picker 承载两个自定义接口、Copilot 和全部可运行的 models.dev 预置。
- 显示始终可见的搜索文本框，直接键入即过滤建议，方向键改变建议项，回车提交当前建议。
- 保持非 TTY stdin 回落路径可脚本化，并允许直接输入查询文本后再按编号选择。
- 根据当前配置高亮对应候选，重新运行 configure 时不把用户带到无关入口。
- catalog 缺失时仍能配置自定义接口或 Copilot。

**非目标：**

- 不修改模型选择器的 `/` 过滤交互；本次直接输入只要求覆盖统一 Provider 入口。
- 不新增模糊匹配、拼音匹配、最近使用排序或联网刷新。
- 不改变 Copilot 设备鉴权、自定义 API 字段、连接测试、模型选择、保存确认或配置 schema。
- 不重新启用当前顶层菜单未暴露的 Codex Provider。

## 决策

### 1. 用带类型的候选模型统一四条路径

在 `configure_catalog` 中增加 `ConfigureProviderChoice`，以枚举区分 `CustomOpenAI`、`CustomAnthropic`、`Copilot` 和 `Catalog`，并携带展示文本及可选 `ProviderEntry*`。纯函数按固定顺序构造候选：

1. 自定义 OpenAI 兼容 API；
2. 自定义 Anthropic 兼容 API；
3. GitHub Copilot；
4. models.dev 中所有 `openai_compat_providers()`。

选择结果使用枚举路由到现有配置函数，避免依赖显示文本或脆弱的数字下标。

替代方案是继续保留四项顶层菜单，只给第二层加搜索框；这不能满足入口合并，也会让用户在搜索 Provider 前多走一步。

### 2. 受管 Copilot 覆盖普通目录项

构造候选时跳过 models.dev 的 `github-copilot` 条目，只保留显式的受管 Copilot 预置。两者虽然共享模型元数据，但鉴权和运行时 Provider 完全不同；若都显示，普通目录项会把用户带到 API Key 流程，既重复又容易配置错误。

Copilot 内部仍可通过 `find_provider("github-copilot")` 读取模型元数据和离线模型回退，不删除 catalog 数据。

### 3. 在通用 picker 增加可选的直接搜索模式

`PickerOptions` 增加 `search_as_you_type` 和搜索提示文本。开启后 picker 在列表上方绘制有边框的输入框，并把普通可打印字符、退格直接解释为过滤输入；过滤继续匹配 `PickerItem.label` 与 `secondary`，因此 Provider ID、展示名和说明都可命中。每次查询改变后高亮第一条建议，方向键、翻页键、Home/End 和回车仍操作列表。

Esc 在查询非空时先清空查询；查询已空时取消整个 picker。此模式不再把 `q`、`c` 或数字解释为命令，确保这些字符可成为查询内容。未开启时保留现有 `/`、`c` 和数字跳转行为，避免影响 Copilot/Codex/model picker。

非 TTY 回落路径无法逐键刷新，因此把任意非命令、非编号文本视为新查询并重印候选；原有 `/<query>` 仍兼容。

### 4. 拆分“选择目录 Provider”和“配置已选 Provider”

把 `configure_openai_via_catalog()` 的后半段提取为接收 `const ProviderEntry&` 的函数。统一 picker 直接把选定目录项交给该函数，不再打开第二个 Provider picker；原有 `run_provider_picker()` 可保留给其他调用者或测试，不参与新顶层流程。

候选 picker 返回取消时，`run_configure()` 立即结束且不保存；只有选定候选后才修改 `cfg` 副本。catalog 分支的 Base URL、env key、模型选择和 `models_dev_provider_id` 行为保持不变。

### 5. 默认高亮来自当前运行时配置

- `provider=anthropic` 高亮第二项；
- `provider=copilot` 高亮第三项；
- `provider=openai` 且 `models_dev_provider_id` 命中目录项时高亮对应预置；
- 其它 `provider=openai` 高亮第一项；
- 未配置或未知 Provider 默认高亮 Copilot，以保持旧向导的初始默认选择。

默认索引由纯函数计算并通过单元测试覆盖。

## 风险 / 权衡

- **[风险] 直接输入模式与现有单键命令冲突** → 仅在统一 Provider picker 开启；该模式下明确取消 `q/c/数字` 命令语义，其他 picker 完全保持旧行为。
- **[风险] catalog 更新后指针失效** → 候选只在一次同步选择调用内持有 `all_providers()` 缓存对象的指针，流程中不触发 refresh；选中后立即消费。
- **[风险] 大目录实时过滤造成重绘开销** → 候选规模仅百余条，使用现有不区分大小写子串过滤即可，无需索引或后台线程。
- **[风险] Copilot 目录项数量看似少一项** → UI 把同一能力显示为受管 Copilot 预置；catalog 数据仍保留给 Copilot 模型元数据，不损失功能。

## 迁移计划

无需配置迁移。发布后运行 `acecode configure` 即进入统一 picker；已有配置仅用于默认高亮。回滚时恢复 `run_configure()` 的四项菜单并关闭 `search_as_you_type`，持久化数据无需转换。

## 待确认问题

无。
