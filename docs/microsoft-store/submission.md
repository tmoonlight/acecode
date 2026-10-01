# ACECode Microsoft Store 提交资料

状态：本地准备中，尚未提交审核。选择 MSIX，首包 x64。账号产品标识尚待用户从 Partner Center 提供；所有待补充字段均为阻塞项，不能把占位文字提交商店。

## 产品设置

- 显示名称：ACECode（以实际预留名称为准）。
- 定位：AI 编程助手、开发者工具。
- 价格建议：客户端免费下载；模型服务可能另行收费，具体以服务提供方为准。
- 语言：简体中文、英语。
- 最低系统：Windows 10 2004（19041），Windows 11。
- 依赖：联网模型需要网络和用户自己的模型配置；Git、Node.js、Python 等按项目需要另行安装。桌面界面使用 WebView2，并保留现有浏览器启动兜底。
- 支持地址：https://github.com/tmoonlight/acecode/issues
- 必填待补充：真实产品 Name、Publisher、PublisherDisplayName、Store ID、发布者/隐私联系渠道、公开 HTTPS 隐私政策 URL。
- 年龄分级：由发布者按真实功能填写问卷；不得凭借“开发者工具”自行代填年龄结论。

## 简体中文

### 简短说明

面向本地项目的 AI 编程助手，支持代码阅读、修改、命令执行与变更检查。

### 详细介绍

ACECode 将 AI 助手与本地开发工作流结合。选择项目目录、连接你使用的模型服务，用自然语言描述任务，让助手协助阅读代码、编辑文件、运行测试并检查变更。

你可以在桌面界面中管理项目与会话，也可以通过 acecode 命令使用终端界面。文件浏览、代码差异和终端输出帮助你理解每一步操作；技能、专家和 MCP 扩展可按工作需要配置。

主要功能：
- 围绕本地工作区阅读和修改代码。
- 运行命令、查看终端输出并验证变更。
- 在桌面和终端界面中使用 AI 助手。
- 管理多个项目、任务和模型配置。
- 按需使用技能、专家、MCP 和浏览器工具。

使用说明：
AI 功能需要可用的模型服务或本地模型配置。第三方模型可能需要账号、API Key 或付费，客户端免费下载不包含模型服务额度。项目所需的编译器和开发工具须按项目要求安装。发起请求时，相关提示、代码、附件及工具结果可能发送至你选择的服务，请在使用前检查模型和权限设置。

### 关键词候选

AI；编程；代码助手；开发者工具；终端；MCP

## English

### Short description

An AI coding assistant for local projects, with code editing, command execution, and change review.

### Description

ACECode connects an AI assistant to your local development workflow. Open a project folder, configure your preferred model service, and describe a task to get help reading code, editing files, running tests, and reviewing changes.

Manage projects and conversations in the desktop app, or use the terminal interface through the acecode command. File browsing, code diffs, and terminal output help you understand the work being performed. Configure skills, experts, MCP integrations, and browser tools as needed.

Features:
- Read and edit code in local workspaces.
- Run commands, inspect output, and validate changes.
- Work through desktop and terminal interfaces.
- Manage projects, tasks, and model configurations.
- Extend workflows with skills, experts, MCP, and browser tools.

AI features require a working model service or local model configuration. Third-party providers may require an account, API key, or payment. A free client download does not include model service credits. Install the development tools required by your projects separately. Relevant prompts, code, attachments, and tool results may be sent to the service you select.

## Notes for certification

ACECode is a native Win32 developer tool packaged as MSIX. The runFullTrust capability is required to read and edit user-selected project folders, execute developer tools, host the local daemon used by the desktop UI, and provide its terminal interface. Computer Use and browser tools are part of the application's user-directed workflows. The package does not install a Windows service or driver.

The desktop entry point is acecode-desktop.exe. The adjacent acecode.exe provides the daemon and CLI; the production command alias is acecode.exe. Windows package installations use Microsoft Store for updates and do not download or replace application binaries using the ordinary ZIP updater.

First-run steps:
1. Launch ACECode from Start.
2. Configure a supported model in Settings.
3. Select a temporary test project and ask the assistant to inspect a sample file.
4. Review the output and exercise a file change or test command with the chosen permission settings.
5. Use Check for updates to confirm the Microsoft Store update message.

Submission blocker: supply a working, limited test account/key or another complete review method through private certification notes. Do not place credentials in this repository or public listing.

## 素材与发布核对

- 包图标：由 assets/windows/acecode_icon.png 生成；300px 商店图标在打包输出的 payload/Assets/StoreListing.png。
- 截图：已从当前 0.9.30 测试包的真实 Desktop/WebView 采集简体中文和英文主页及商店更新提示，保存在 build/store-msix/local-test-26brfbee/store-*.png。没有使用旧版帮助文档截图。
- 已核对模型和反馈数据流。商店版拒绝 HTTP 反馈上传，包内实测确认返回 HTTPS 要求且不上传；发布者仍需提供可用的 HTTPS 反馈服务及其保存/删除规则。普通安装保持原协议兼容。
- 完成原生构建、MSIX 包内运行、命令行别名、Desktop/daemon、资源、更新拦截和卸载验证。
- 取得真实身份后重新构建正式 MSIX；本地测试身份绝不上传。
- 后台提交后登记时间、版本、Store ID 和认证状态；认证通过后检查商店页面和实际安装。

## 官方依据

- https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements
- https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/create-app-submission
- https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations
- https://learn.microsoft.com/en-us/windows/msix/package/create-app-package-with-makeappx-tool
