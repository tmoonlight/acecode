## 1. macOS 凭据与打包边界

- [x] 1.1 将 Application/公证凭据与 Installer 凭据拆分为独立工作流状态，并拒绝部分 Installer 配置
- [x] 1.2 仅在 Installer 凭据完整时导入 Installer 身份并创建、公证、上传 PKG

## 2. Release 资产契约

- [x] 2.1 允许正式 Release 包含零个或两个 PKG，拒绝单架构、unsigned 和 DMG 资产
- [x] 2.2 更新 macOS 发布文档，说明无 PKG 发布与后续自动恢复条件

## 3. 验证

- [x] 3.1 运行 OpenSpec strict validation、工作流契约检查、Shell 语法检查和 `git diff --check`
- [ ] 3.2 在无 Installer Secret 的 GitHub Actions 干跑中确认可信 macOS 资产成功且没有 PKG artifact
