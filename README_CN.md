<p align="center">
  <img src="assets/branding/acecode-icon.svg" width="96" alt="ACECode 图标">
</p>

<h1 align="center">ACECode</h1>

<p align="center">
  <strong>同时面向桌面与终端的 AI 编程智能体。</strong><br>
  理解代码库、完成修改、运行命令，并通过可随时恢复的任务组织开发工作。
</p>

<p align="center">
  <a href="https://github.com/tmoonlight/acecode/stargazers"><img src="https://img.shields.io/github/stars/tmoonlight/acecode?style=flat-square" alt="GitHub Stars"></a>
  <a href="https://github.com/tmoonlight/acecode/network/members"><img src="https://img.shields.io/github/forks/tmoonlight/acecode?style=flat-square" alt="GitHub Forks"></a>
  <a href="https://github.com/tmoonlight/acecode/issues"><img src="https://img.shields.io/github/issues/tmoonlight/acecode?style=flat-square" alt="GitHub Issues"></a>
  <a href="https://github.com/tmoonlight/acecode/commits"><img src="https://img.shields.io/github/last-commit/tmoonlight/acecode?style=flat-square" alt="最近提交"></a>
</p>

<p align="center">
  <a href="README.md">English</a> | <strong>中文</strong>
</p>

<p align="center">
  <a href="#认识-acecode">界面预览</a> &bull;
  <a href="#快速开始">快速开始</a> &bull;
  <a href="#核心能力">核心能力</a> &bull;
  <a href="#进一步了解">进一步了解</a>
</p>

ACECode 是一个能够理解项目上下文的 AI 编程智能体，提供可视化 Desktop 和键盘优先的终端 TUI 两套完整界面。两者共享同一套智能体核心、模型配置、权限系统、内置工具、Skills 与 MCP 扩展能力。

## 认识 ACECode

### Desktop

在一个窗口中选择工作区、创建或回到已有任务、切换模型与权限、添加上下文，并持续查看智能体的工作过程。

![ACECode Desktop 首页，展示工作区、任务、模型和权限控件](assets/readme/acecode-desktop.png)

### 终端 TUI

直接在项目终端中工作，通过流式回复、工具进度、会话控制和键盘操作保持专注。

![ACECode 终端 TUI，展示编程对话与任务状态](assets/readme/acecode-tui.png)

## 快速开始

### Desktop

1. 打开 ACECode Desktop。
2. 首次使用时，进入 **设置 > 模型**，配置服务商和模型。
3. 选择 **新建任务**，然后选择已有工作区、打开本地项目，或选择 **不使用工作区** 处理通用任务。
4. 在输入框下方选择模型和权限模式。
5. 描述想要的结果，按 `Enter` 发送。

> [!TIP]
> 输入 `@` 可以引用工作区内的文件或目录；需要更多上下文时，可通过添加按钮附加图片、文件或文件夹。

### 终端 TUI

首次使用时先配置模型：

```bash
acecode configure
```

进入希望 ACECode 操作的项目目录并启动：

```bash
cd /path/to/your/project
acecode
```

输入一个目标明确的任务，按 `Enter` 发送：

```text
先分析这个仓库的会话存储方式，再为序列化逻辑补一个聚焦测试。
```

下次回来时，可以恢复当前项目最近的任务：

```bash
acecode --resume
```

> [!IMPORTANT]
> 默认权限模式下，ACECode 通常会自动读取项目上下文，并在敏感写入或执行命令前请求确认。接受前请检查权限请求、工具输出和文件改动。

## 适合从这些任务开始

- `说明项目架构，并指出最重要的入口文件。`
- `定位这个失败测试的原因，先给出最小修复方案。`
- `重构 @src/session/，不要改变公开行为，然后运行相关测试。`
- `审查当前差异，检查正确性、回归风险和缺少的测试。`

好的任务描述通常会说清楚预期结果、相关文件或限制，以及完成后应如何验证。

## 核心能力

- **理解代码库** — 搜索代码、追踪调用链、检查配置，并解释陌生系统。
- **安全修改代码** — 编辑文件、展示差异、运行命令与测试，并在需要时请求权限。
- **延续任务上下文** — 持久化任务与会话、恢复历史工作、压缩长对话并回看记录。
- **自由选择界面** — 在 Desktop 中可视化管理多个工作区，或在 TUI 中贴近终端工作流。
- **使用自己的模型** — 支持 GitHub Copilot、OpenAI 兼容 API、Anthropic 和已保存的模型配置。
- **扩展智能体能力** — 通过 Skills、MCP 服务器、工具、钩子与连接器适配专业工作流。

## 常用 TUI 命令

| 命令 | 用途 |
| --- | --- |
| `/help` | 查看当前安装版本支持的命令。 |
| `/model` | 查看或切换当前模型。 |
| `/resume` | 打开会话选择器。 |
| `/skills` | 打开 Skills 能力中心。 |
| `/mcp` | 打开 MCP 服务器能力中心。 |
| `/exit` | 退出 ACECode。 |

## 进一步了解

- [使用手册](docs/user-manual.md) — TUI 日常操作、权限、会话、Skills 与 MCP。
- [架构说明](ARCHITECTURE.md) — 运行界面、共享核心与源码归属。
- [Skills 指南](docs/skills.md) — 创建和使用可复用工作流。
- [Desktop 工作区](docs/desktop-shell/multi-workspace.md) — 桌面应用中的工作区与任务行为。
- [Linux 自升级](docs/linux-self-update.md) — updater ZIP、旧版一次性引导与发布校验。
