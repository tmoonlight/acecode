## 为什么

`acecode configure` 目前先让用户在四个入口中选择，再进入 models.dev Provider 列表；目录搜索还要求先按 `/`，使“选择预置 Provider”和“配置自定义接口”成为两套割裂流程。需要把这些入口收拢为一个可直接输入并实时给出候选项的选择器，让用户既能快速选预置，也能明确进入 OpenAI 或 Anthropic 风格的自定义接口配置。

## 变更内容

- 把现有 Copilot、models.dev 目录、自定义 OpenAI 兼容接口和 Anthropic 接口四个入口合并为一个 Provider 选择器。
- 将“自定义 OpenAI 兼容 API”和“自定义 Anthropic 兼容 API”固定放在列表第一、第二位；选中后分别复用现有第 3、4 项配置流程。
- 将 GitHub Copilot 作为受管预置项放入同一列表，并继续复用现有设备登录和模型选择流程。
- 在其后追加可运行的 models.dev 预置 Provider；排除会与受管 Copilot 重复且鉴权语义不同的普通 `github-copilot` 目录项。
- 为统一选择器显示文本输入框；用户直接键入即可实时过滤候选并用方向键、回车完成 autocomplete 式选择，不再要求先按 `/`。
- catalog 缺失时仍提供两个自定义接口和 Copilot，不再显示一个不可用的中间入口。

## 能力

### 新增能力

- `tui-configure-provider-selection`：定义 `acecode configure` 的统一 Provider 候选集合、固定顺序、直接输入 autocomplete、当前配置默认高亮及各候选项到既有配置流程的路由。

### 修改能力

（无；当前主规格目录中尚无 configure Provider 选择能力。）

## 影响

- 主要修改 `src/commands/configure.cpp`、`src/commands/configure_catalog.{hpp,cpp}` 和 `src/commands/configure_picker.{hpp,cpp}`。
- 扩展 `tests/commands/configure_picker_test.cpp` 与 `tests/commands/configure_catalog_test.cpp`，覆盖候选顺序、重复项消除、默认高亮和过滤语义。
- 不改变配置文件 schema、models.dev 数据格式、Copilot/自定义接口的鉴权流程或模型保存结构。
