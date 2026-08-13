## ADDED Requirements

### Requirement: 正式标签必须保留可信 macOS 应用资产
正式标签在缺少 `Developer ID Installer` 凭据时，工作流 MUST 继续使用 `Developer ID Application` 签名应用和可执行文件，完成应用公证与 stapling，并只发布通过可信校验的 macOS 更新 ZIP 与归档。

#### Scenario: Installer 凭据缺失但应用凭据完整
- **WHEN** 正式标签具有完整 Application、公证凭据，但两项 Installer Secret 均未配置
- **THEN** 两个 macOS 架构任务完成应用签名、公证、stapling 和可信更新 ZIP 生成
- **THEN** 工作流不创建、不上传、不发布任何 PKG

#### Scenario: 应用签名或公证凭据缺失
- **WHEN** 正式标签缺少 Application 证书或 Apple 公证所需的任一凭据
- **THEN** macOS 任务 MUST 在发布资产前失败
- **THEN** 工作流不得发布 unsigned 更新 ZIP 或归档

### Requirement: PKG 发布必须保持双架构和完整信任链
工作流仅在两项 Installer Secret 均存在时 SHALL 创建 PKG，并 MUST 对 x64 与 arm64 PKG 完成 Installer 签名、公证、stapling、签名检查和 Gatekeeper 安装评估。

#### Scenario: Installer 凭据完整
- **WHEN** Application、公证与 Installer 凭据全部完整
- **THEN** Release 包含名称精确匹配版本与 x64、arm64 架构的两个可信 PKG

#### Scenario: Installer 凭据只配置一项
- **WHEN** `MACOS_INSTALLER_CERTIFICATE_BASE64` 与 `MACOS_INSTALLER_CERTIFICATE_PASSWORD` 仅有一项存在
- **THEN** 工作流 MUST 将其视为配置损坏并失败

### Requirement: Release 资产收集必须拒绝不完整或不可信安装器
正式 Release MUST 拒绝 DMG、带 `-unsigned` 后缀的 PKG、带 `-unsigned` 后缀的 macOS 更新 ZIP，以及仅有一个架构 PKG 的资产集合；PKG 数量只允许为零或恰好两个。

#### Scenario: 本次发布省略 PKG
- **WHEN** 两个 macOS 任务均未上传 PKG
- **THEN** Release 收集阶段继续发布其余可信平台资产

#### Scenario: PKG 集合不完整
- **WHEN** 收集阶段只找到一个 PKG，或两个文件未精确覆盖 x64 与 arm64
- **THEN** Release 任务 MUST 失败且不得创建 GitHub Release

#### Scenario: 出现 unsigned 或旧 DMG 资产
- **WHEN** 收集阶段发现 unsigned PKG、unsigned macOS 更新 ZIP 或 DMG
- **THEN** Release 任务 MUST 失败且不得发布这些资产
