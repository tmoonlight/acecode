## 为什么

`acecode configure` 目前先让用户在四个入口中选择，再进入 models.dev Provider 列表；目录搜索还要求先按 `/`，使“选择预置 Provider”和“配置自定义接口”成为两套割裂流程。需要把这些入口收拢为一个可直接输入并实时给出候选项的选择器，让用户既能快速选预置，也能明确进入 OpenAI 或 Anthropic 风格的自定义接口配置。Web 模型目录还额外提供 ACEModel，但其内置元数据没有进入 TUI 候选，因此统一列表也必须同步这一自营预置。

## 变更内容

- 把现有 Copilot、models.dev 目录、自定义 OpenAI 兼容接口和 Anthropic 接口四个入口合并为一个 Provider 选择器。
- 将“自定义 OpenAI 兼容 API”和“自定义 Anthropic 兼容 API”固定放在列表第一、第二位；选中后分别复用现有第 3、4 项配置流程。
- 将 ACEModel 作为第一个内置预置放在第三位，将 GitHub Copilot 作为受管预置放在第四位；分别复用 OpenAI 兼容预置流程和现有设备登录流程。
- 在其后追加可运行的 models.dev 预置 Provider；排除会与受管 Copilot 重复且鉴权语义不同的普通 `github-copilot` 目录项。
- 把 ACEModel 的 ID、名称、固定 Base URL、API Key 环境变量及内置模型抽成 Web 与 TUI 共用的定义，并排除未来 catalog 中可能出现的同 ID 重复项。
- 为统一选择器显示文本输入框；用户直接键入即可实时过滤候选并用方向键、回车完成 autocomplete 式选择，不再要求先按 `/`。
- catalog 缺失时仍提供两个自定义接口、ACEModel 和 Copilot，不再显示一个不可用的中间入口。

## 能力

### 新增能力

- `tui-configure-provider-selection`：定义 `acecode configure` 的统一 Provider 候选集合、固定顺序、直接输入 autocomplete、ACEModel 共享元数据、当前配置默认高亮及各候选项到既有配置流程的路由。

### 修改能力

（无；当前主规格目录中尚无 configure Provider 选择能力。）

## 影响

- 主要修改 `src/commands/configure.cpp`、`src/commands/configure_catalog.{hpp,cpp}`、`src/commands/configure_picker.{hpp,cpp}` 与 `src/web/handlers/model_catalog_handler.cpp`，并新增共享 ACEModel catalog 定义。
- 扩展命令、共享 Provider 与 Web catalog 测试，覆盖候选顺序、重复项消除、默认高亮、过滤语义及元数据一致性。
- 不改变配置文件 schema、models.dev 数据格式、Copilot/自定义接口的鉴权流程或模型保存结构。
