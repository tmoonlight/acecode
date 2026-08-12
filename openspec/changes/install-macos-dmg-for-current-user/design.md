## Context

当前 DMG 使用静态符号链接 `Applications -> /Applications`，因此拖放会写入系统级目录。分发型 DMG 无法可靠地创建指向下载用户 `~/Applications` 的静态符号链接：`~` 和环境变量不会在 Finder 解析符号链接时展开，而在打包阶段展开 `$HOME` 又会固化 CI 运行器的主目录。

仓库已有严格的当前用户安装路径辅助函数与兼容 `~/Applications/ACECode.app` 的自更新逻辑。实现需要重新提供一个小型 AppKit 安装入口，把目标路径留到下载用户运行时解析，并继续通过现有 Developer ID 与公证流程。

## Goals / Non-Goals

**Goals:**

- DMG 中的拖放安装只写入当前用户的 `~/Applications/ACECode.app`。
- 维持左侧 `ACECode.app`、右侧 `Applications` 的简洁两项布局。
- 不请求管理员权限，不调用 `sudo`，不写入 `/Applications`。
- 验证来源、Bundle ID、目标目录和替换路径，避免符号链接重定向或安装任意应用。
- 当前用户安装入口与主应用一起构建、签名、检查和打包。

**Non-Goals:**

- 自动迁移或删除已经存在的 `/Applications/ACECode.app`。
- 修改 macOS 自更新支持的两个既有目标，或扩大到任意安装目录。
- 引入特权辅助进程、`.pkg` 安装包或新的外部依赖。
- 改动 Windows、Linux 或更新清单格式。

## Decisions

### 使用运行时安装入口而不是静态用户目录链接

DMG 右侧将放置签名的 `Applications.app`，Finder 默认显示为 `Applications`。它既接受从同一 DMG 拖入 `ACECode.app`，也允许直接打开，并在运行时通过 `NSHomeDirectory()` 解析当前用户主目录。

静态 `~/Applications` 链接不可用，因为 POSIX 符号链接不展开 `~`；打包时展开 `$HOME` 会指向构建用户。可执行安装入口虽然比符号链接多一个小型 bundle，但能为任意下载用户提供确定且可审计的目标。

### 复用严格的当前用户路径策略

安装入口复用 `macos_user_install_paths()` 和 `macos_user_install_destination_is_safe()`。它要求 `~/Applications` 与现有 `ACECode.app` 都是真实目录，解析后目标必须精确等于当前用户主目录下的 `Applications/ACECode.app`，并拒绝其他拖入来源、错误 Bundle ID、符号链接或正在运行的 ACECode。

替换时先复制到同目录的随机隐藏临时 bundle，再使用同卷替换或移动；失败时清理临时项。该方案沿用已经验证过的当前用户安装边界，并避免部分覆盖现有应用。

### 保持两项式布局并明确真实语义

DMG 不再包含 `/Applications` 符号链接，也不增加可见说明文件。右侧 bundle 的显示名为 `Applications`，构建时直接使用当前 macOS 提供的标准 `ApplicationsFolderIcon.icns`，使其与常规 DMG 的 Applications 文件夹目标保持一致，不再复用 ACECode 品牌图标或叠加下载徽标。背景仍只显示品牌与左到右箭头。布局测试必须同时断言用户安装入口存在、使用系统 Applications 文件夹图标以及系统链接不存在。

### 将安装入口纳入发布信任链

CMake 生成 `Applications.app`；发布工作流构建并剥离其符号，使用与主应用相同的 Developer ID 指纹签名，再将其传给 DMG 脚本。最终 DMG 继续整体签名、公证、装订和 Gatekeeper 检查。手动无凭据构建仍可生成明确标记的未签名检查包。

## Risks / Trade-offs

- **右侧项目技术上是应用而不是文件夹** → 使用 macOS 自带的 Applications 文件夹图标维持标准 DMG 视觉语义，并在文档说明拖放或双击行为。
- **恢复安装入口增加构建与签名表面** → 保持实现独立、仅链接 AppKit/Foundation，并在工作流和脚本测试中强制构建及签名参数。
- **现有系统级副本可能与用户级副本并存** → 只安装和启动当前用户副本，不自动删除系统副本；后续更新仍只更新实际运行的受支持副本。
- **目标目录可能被符号链接重定向** → 创建或验证真实目录并在复制前、替换前检查规范化后的精确目标。

## Migration Plan

1. 增加当前用户安装入口及其 CMake 目标，并接入发布签名流程。
2. 用该入口替换 DMG 中的 `/Applications` 符号链接，同时更新契约测试和文档。
3. 在 macOS 上构建、签名并挂载检查 DMG，验证拖放后文件只出现在 `~/Applications`。
4. 若发布验证失败，回退本变更即可恢复系统 `/Applications` 链接；已有用户级和系统级安装均不受数据迁移影响。

## Open Questions

- 无。
