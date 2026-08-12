## Why

当前 DMG 的 `Applications` 拖放目标固定指向系统级 `/Applications`，普通用户安装和后续自更新可能需要管理员权限。新的安装入口应始终把 ACECode 安装到打开 DMG 的当前用户目录 `~/Applications/ACECode.app`，且不能把打包机的主目录写入发布物。

## What Changes

- **BREAKING** 将 DMG 右侧的系统 `/Applications` 符号链接替换为经过签名的当前用户安装入口。
- 保留两项式拖放布局：用户把 `ACECode.app` 拖到右侧 `Applications` 项后，安装入口将其安全复制到 `~/Applications/ACECode.app`。
- 安装入口按运行时用户解析主目录，创建真实的 `~/Applications` 文件夹，并拒绝符号链接、重定向路径及非 DMG 内置的应用来源。
- 将当前用户安装入口纳入 CMake 构建、Developer ID 签名、符号处理和 DMG 发布流程。
- 更新脚本契约测试与 macOS 发布说明，明确不写入系统 `/Applications`、不调用 `sudo`、不请求管理员权限。

## Capabilities

### New Capabilities

- `macos-current-user-dmg-installation`: 规定 macOS DMG 通过可拖放的当前用户安装入口，将内置 ACECode 安全安装到 `~/Applications/ACECode.app`。

### Modified Capabilities

- 无。

## Impact

- DMG 打包与布局：`scripts/macos_create_dmg.sh`、macOS 安装入口资源及布局测试。
- macOS 构建与发布：`cmake/acecode_desktop.cmake`、`.github/workflows/package.yml`、签名与发布脚本契约测试。
- 安装安全边界：当前用户安装入口及已有的 `src/desktop/user_install_policy.*` 路径校验。
- 文档：`docs/macos-release.md`；Windows、Linux、更新清单格式和 Apple 凭据不变。
