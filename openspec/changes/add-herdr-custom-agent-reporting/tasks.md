## 1. 通用生命周期事件

- [x] 1.1 为 `PermissionResolved` 新增先失败的载荷与 agent-loop 测试。
- [x] 1.2 新增事件常量与通用载荷构造器。
- [x] 1.3 对 hook、interactive、headless 与 implicit 决定恰好派发一次解决事件。
- [x] 1.4 将 `PermissionResolved` 注册到裸 Codex hook 对象识别逻辑，并补回归测试。
- [x] 1.5 覆盖 interactive always-allow、headless deny 与 implicit allow 的来源归因。

## 2. 可选配置

- [x] 2.1 新增一个跨平台 Herdr hooks JSON 示例。
- [x] 2.2 记录安装、信任审查、状态映射与限制。
- [x] 2.3 通过假 Herdr 可执行文件验证示例。

## 3. 验证

- [x] 3.1 运行 hook runtime 与 agent-loop 测试。
- [x] 3.2 构建相关测试目标与 ACECode 目标。
- [x] 3.3 运行格式/diff 检查，并审查最终范围是否引入 Herdr runtime coupling。

## 4. 默认 seed 集成

- [x] 4.1 为 hook seed 安装、版本升级、幂等、用户改动保护与用户配置隔离补测试。
- [x] 4.2 把默认 hook 纳入通用 seed reconcile，并在首次 registry 加载前完成启动接线。
- [x] 4.3 对 seed hook 做官方定义指纹校验，只把匹配内容标记为 `ManagedTrusted`。
- [x] 4.4 同步 `MANIFEST.json`、`seed.version`、默认资源文档与打包元数据。
- [x] 4.5 运行 focused tests、构建、严格 OpenSpec 校验与跨平台假 Herdr 验证。

## 5. 真实 Herdr pane 回归

- [x] 5.1 记录 Windows managed pane 缺少 `HERDR_BIN_PATH` 时静默跳过的实机根因，并明确 pane 身份边界。
- [x] 5.2 为 `HERDR_BIN_PATH` 缺失、CLI fallback 与 `HERDR_PANE_ID` 原样传递新增先失败的回归测试。
- [x] 5.3 修正 POSIX 与 Windows seed 命令的 Herdr CLI 解析，同时保持 Herdr 外部无副作用。
- [x] 5.4 同步示例、seed 版本、官方指纹、manifest、安装升级测试与文档。
- [x] 5.5 运行 focused/full 测试、构建、严格 OpenSpec 校验，并用真实 Windows Herdr CLI 验证 agent 列表联动。

## 6. Windows cmd 引号实机回归

- [x] 6.1 新增先失败的 Windows 回归测试，精确校验 hook runner 传给 Herdr 的 pane ID 不带反斜杠或额外引号。
- [x] 6.2 修正 `cmd.exe /d /s /c` 的命令行构造，不再对 shell 命令正文应用普通 C argv 引号规则。
- [x] 6.3 运行 focused/full 测试、构建、严格 OpenSpec 校验，并在真实 Herdr pane 中确认 SessionStart 自动出现在 agents 区域。

## 7. 快速升级补种回归

- [x] 7.1 新增先失败的回归测试，覆盖用户 marker 已等于包内版本、托管 `agent-reporting` 目录却缺失的状态。
- [x] 7.2 同版本只对缺失或已知旧官方托管 hook 触发修复 reconcile，提升 seed revision，并保持用户修改资源不被覆盖。
- [x] 7.3 构建并发布新的 Windows QuickValidation 包，验证旧 seed 用户首次启动后自动安装并以 `ManagedTrusted` 加载 hook，同时校验 ZIP、manifest、SHA256 与 HTTP。
