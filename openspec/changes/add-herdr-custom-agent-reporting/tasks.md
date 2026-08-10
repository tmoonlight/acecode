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
