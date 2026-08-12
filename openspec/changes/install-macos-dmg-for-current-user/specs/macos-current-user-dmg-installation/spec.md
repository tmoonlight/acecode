## ADDED Requirements

### Requirement: DMG 提供当前用户安装目标
macOS DMG SHALL 仅显示 `ACECode.app` 与名为 `Applications` 的当前用户安装入口。该入口 MUST 使用 macOS 标准 Applications 文件夹图标，MUST NOT 复用 ACECode 主应用图标，并 SHALL NOT 包含指向系统 `/Applications` 的符号链接。

#### Scenario: 用户拖放主应用
- **WHEN** 用户把同一 DMG 中的 `ACECode.app` 拖到 `Applications` 安装入口
- **THEN** 安装入口把该应用安装到运行用户的 `~/Applications/ACECode.app`
- **THEN** 系统 `/Applications` 不被写入

#### Scenario: 用户直接打开安装入口
- **WHEN** 用户直接打开 DMG 中的 `Applications` 安装入口
- **THEN** 安装入口使用同一 DMG 内相邻的 `ACECode.app` 作为唯一安装来源
- **THEN** 安装结果仍位于运行用户的 `~/Applications/ACECode.app`

#### Scenario: Finder 显示安装布局
- **WHEN** 用户在 Finder 中打开 DMG
- **THEN** 左侧显示 ACECode 主应用图标，右侧显示 macOS 标准 Applications 文件夹图标
- **THEN** 右侧不得显示 ACECode 主应用图标或 ACECode 下载徽标变体

### Requirement: 用户目录在安装时解析
安装入口 MUST 在下载用户运行时解析当前用户主目录，并 MUST NOT 把打包机主目录、文字 `~` 或未展开的环境变量作为发布目标。

#### Scenario: 不同用户打开同一 DMG
- **WHEN** 两个主目录不同的用户分别运行同一个发布 DMG
- **THEN** 每次安装都以各自运行用户的 `~/Applications/ACECode.app` 为目标

### Requirement: 安装路径保持用户级且不可重定向
安装入口 MUST 创建或验证真实的 `~/Applications` 目录，并 MUST 拒绝会把该目录或目标应用重定向到其他位置的符号链接及非目录项。

#### Scenario: 用户 Applications 目录不存在
- **WHEN** 当前用户主目录下不存在 `Applications`
- **THEN** 安装入口创建真实的 `~/Applications` 目录后完成安装

#### Scenario: 用户 Applications 目录是符号链接
- **WHEN** `~/Applications` 是符号链接或解析后不再是当前用户主目录下的精确目标
- **THEN** 安装入口在复制或替换 ACECode 前停止并显示错误

### Requirement: 安装来源与替换过程经过验证
安装入口 MUST 只接受同一 DMG 中预期的 `ACECode.app`，MUST 验证其 Bundle ID，并 MUST 通过同目录临时 bundle 完成安全替换。

#### Scenario: 拖入其他应用
- **WHEN** 用户把不是同一 DMG 内置 `ACECode.app` 的应用拖到安装入口
- **THEN** 安装入口拒绝该来源且不修改现有安装

#### Scenario: 替换现有用户级安装
- **WHEN** `~/Applications/ACECode.app` 是真实目录、ACECode 未运行且新 bundle 验证通过
- **THEN** 安装入口先复制到隐藏临时 bundle，再原位替换现有应用并启动新副本

### Requirement: 安装不需要管理员权限
当前用户安装流程 SHALL NOT 调用 `sudo`、Authorization Services 或其他特权辅助进程，并 SHALL NOT 请求对系统 `/Applications` 的写权限。

#### Scenario: 标准用户完成安装
- **WHEN** 没有管理员权限的用户拥有自己主目录的正常写权限
- **THEN** 用户能够完成安装且不会出现管理员授权提示

### Requirement: 安装入口属于发布信任链
发布工作流 MUST 构建并使用与主应用相同的 Developer ID 身份签名当前用户安装入口，最终 DMG MUST 继续经过既有签名与 Apple 公证流程。

#### Scenario: 创建带签名发布 DMG
- **WHEN** macOS 发布凭据完整且工作流创建正式 DMG
- **THEN** 主应用与当前用户安装入口均已签名并包含在提交公证的 DMG 中
