## Purpose

为 ACECode 提供可以从 Microsoft Store 安装和更新的 Windows 桌面包，保留桌面、终端与模型资源的一致性，并保证包身份、提交材料及验证结果均可追溯到实际产物。

## ADDED Requirements

### Requirement: 商店包使用真实产品身份
正式提交包 SHALL 使用账号产品标识中的 Name、Publisher 和 PublisherDisplayName，缺失标识时 MUST 拒绝生成正式提交产物。

#### Scenario: 缺少产品身份
- **WHEN** 正式打包未提供必需身份字段
- **THEN** 构建失败并说明缺少字段，不生成可误认的正式包

#### Scenario: 本地测试身份
- **WHEN** 开发者显式选择本地测试模式
- **THEN** 允许生成独立测试身份的包，并在文件名和报告标明不能提交商店

### Requirement: 桌面与命令行入口完整
MSIX SHALL 包含 Desktop、CLI、Computer Use 和所需运行资源，提供开始菜单桌面入口及 acecode.exe 命令行别名。

#### Scenario: 用户从商店安装
- **WHEN** 用户安装包并启动 ACECode
- **THEN** 桌面能够启动相邻后台，模型目录和 seed 从包中解析，用户数据写入用户目录

#### Scenario: 命令行启动
- **WHEN** 用户通过应用执行别名运行 acecode
- **THEN** CLI 使用安装包中的程序并接收原始参数

### Requirement: 产物与验证可追溯
打包 SHALL 记录项目版本、包版本、包身份、源码提交、文件清单及 SHA-256，拒绝缺失核心程序或架构不一致的输入。

#### Scenario: 架构不一致
- **WHEN** x64 包输入包含 ARM64 核心程序
- **THEN** 打包在生成 MSIX 前失败并指出不匹配文件

### Requirement: 商店反馈使用加密传输
商店安装版 SHALL 在反馈上传前要求 HTTPS 地址，拒绝明文传输并明确告知未上传；普通安装保持既有可配置协议行为。

#### Scenario: 反馈服务使用 HTTP
- **WHEN** MSIX 用户提交反馈且服务地址为 HTTP
- **THEN** 应用不发送日志或会话，提示配置 HTTPS 服务后再试

### Requirement: 提交资料准确且完整
商店提交 SHALL 提供真实功能说明、隐私政策、支持联系方式、权限用途和审核测试方式，未完成事项 MUST 保留为待办。

#### Scenario: 仅本地验证成功
- **WHEN** MSIX 已生成但尚未提交或审核
- **THEN** 交付记录仅标记本地完成，不宣称上架成功
