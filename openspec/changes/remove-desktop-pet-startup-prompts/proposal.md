## Why

用户决定正式删除开启桌面宠物、虚拟办公室的首屏提示，后续版本也不再展示。现有首次邀请和升级宠物推广仍由原生偏好状态自动触发，需要完整退役。

## What Changes

- 删除首次邀请、宠物推广弹框及其预览和自动认领流程。
- 移除版本门槛、已看过记录和对应原生桥接，首屏不再等待这些提示。
- 保留设置页、快捷菜单、宠物选择、默认关闭及已有用户的启用状态。

## Capabilities

### New Capabilities

- `virtual-office-preferences`: 明确桌面宠物首屏无推广提示及旧偏好兼容行为。主规格尚无此能力；本次决定覆盖未归档变更 `add-virtual-office-preferences` 的 `One-time invitation` 要求。

### Modified Capabilities

无。

## Impact

涉及 Web 应用启动协调、桌面宠物偏好、Windows/macOS 共享服务及相关回归测试。不改变 daemon API。
