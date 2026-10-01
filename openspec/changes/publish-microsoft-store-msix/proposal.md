## Why

用户要求将 ACECode 发布到 Microsoft Store，已有开发者账号但尚未创建产品，并确认采用由商店签名的 MSIX。当前只有普通 Windows ZIP 和 Inno Setup 安装器，自更新器会替换安装目录，不能直接用于受保护的 MSIX 安装。

## What Changes

- 增加 Windows x64 MSIX 打包工具，复用 Release 产物、官方图标、模型目录和 seed，包含 Desktop、CLI 和 Computer Use。
- 显式注入账号产品标识；缺少真实标识时只允许生成明确标注的本地验证包。
- 声明 full-trust 桌面入口和命令行别名，不使用安装向导或安装期 PowerShell。
- MSIX 的检查更新和执行升级在网络请求前返回商店管理状态，Web/Desktop 显示商店更新说明；普通安装版行为不变。
- 准备中英文商店资料、隐私说明、图标和验证记录，分别登记本地完成、正式提交和审核结果。

## Capabilities

### New Capabilities

- `microsoft-store-package`: MSIX 产品标识、完整资源、入口与提交资料。

### Modified Capabilities

- `self-upgrade`: MSIX 安装使用商店更新，不自行替换程序文件。

## Impact

影响 `installer/windows/store/`、`src/adapters/upgrade/`、Windows 包身份检测边界、更新状态 API/前端提示、相关测试和 `docs/microsoft-store/`。使用本机 Windows SDK；最终提交依赖账号标识。保持当前 master，保留已有跨平台补验相关未提交文件。
