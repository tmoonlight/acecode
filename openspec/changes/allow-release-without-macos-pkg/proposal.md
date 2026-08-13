## 为什么

当前正式标签把 macOS 应用签名凭据和 PKG Installer 凭据绑定为一个整体；缺少 `Developer ID Installer` 时，Windows、Linux 以及本可正常签名和公证的 macOS 更新包都会被阻断。v0.8.16 需要先发布其余可信平台资产，同时明确禁止用未签名 PKG 充当正式安装包。

## 变更内容

- 将 macOS 应用签名/公证凭据与 PKG Installer 凭据拆分校验。
- 正式标签始终要求 macOS 应用完成 `Developer ID Application` 签名、公证和 stapling，继续生成可信更新 ZIP 与归档。
- 未配置完整 Installer 凭据时不创建、不上传、不发布 PKG；配置完整时仍生成两个已签名并公证的架构 PKG。
- 正式 Release 只接受零个或恰好两个可信 PKG，继续拒绝 DMG、unsigned PKG 和 unsigned 更新 ZIP。

## 能力

### 新增能力

- `macos-optional-pkg-release`: 定义正式标签在缺少 Installer 凭据时省略 PKG、但仍强制发布可信 macOS 应用资产的契约。

### 修改能力

无。

## 影响

- `.github/workflows/package.yml` 的 macOS 凭据检查、证书导入、PKG 构建/上传和 Release 资产收集。
- `docs/macos-release.md` 的凭据、干跑和正式标签说明。
- macOS 发布契约测试与 GitHub Release 资产集合。
