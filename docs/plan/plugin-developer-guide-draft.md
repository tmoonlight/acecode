# ACECode 插件开发说明书（假想草案）

> 草案，2026-11-26。这是一份「假如插件机制已经做出来」的开发者文档，用来从开发者的角度检验方案是否靠谱，**不是现状说明**。
>
> - 【现有】：今天的 ACECode 已经这样工作，后面给出源码或文档位置。
> - 【新增】：方案要新做的部分，今天不存在。
> - 第 12 节「写这份说明书时发现的坑」是这次练习最主要的产出。

## 0. 写这份草案时采用的假设

上一轮讨论留下四个决策点，你还没拍板。本文按推荐项写，任何一条被否，相关章节都要改：

1. 插件格式兼容 Claude Code 的目录约定。ACECode 独有的东西写在 `.acecode-plugin/plugin.json` 里。
2. 不自动读取 Claude Code / Codex 已经装好的插件，只提供「导入」。
3. 消息平台只走 `acecode.channel-platform.v1` 协议，`/rc` 渠道插件协议（`acecode.channel-plugin.v1`）标为旧接口。
4. Claude 插件里的 `agents/*.md` 读成「只有一个角色的专家」。

---

## 1. 插件是什么

插件就是一个文件夹。文件夹里放一份说明文件，再按约定的子目录放几样东西：技能、命令、钩子、MCP 服务器、专家、消息平台。

插件**本身不提供新能力**，它只做打包这件事：别人可以一次装好、一次删干净，也能整体升级。真正干活的是里面的「组件」，而每种组件今天在 ACECode 里都已经存在。

### 该用哪种组件

| 你想要 | 用 | 它本质上是 |
|---|---|---|
| 让模型学会一套做事流程 | 技能 | 一份 Markdown，模型按需读 |
| 用户输入 `/xxx` 触发一段固定提示 | 命令 | 一份 Markdown 模板（在 ACECode 里，命令就是技能） |
| 让模型能调用外部系统 | MCP 服务器 | 一个子进程或一个 HTTP 服务 |
| 在某个时机自动做检查、补上下文或拦截操作 | 钩子 | 一条命令，事件 JSON 从标准输入传进去 |
| 换一个角色或一组角色来干活 | 专家 | `expert.json` 加若干角色 Markdown |
| 从某个聊天软件收发任务 | 消息平台 | 一个常驻子进程，逐行收发 JSON |
| 装好后要先登录、换取密钥 | 设置程序 | 一个一次性子进程 |
| 把以上几样打包，让别人一键安装 | **插件** | 上面这些组件的容器 |

不属于插件的东西：主题（走主题包和工坊）、模型配置、工具重写、项目指令 `AGENTS.md`。

### 一条原则

**ACECode 不把第三方代码加载进自己的进程。** 插件里凡是要运行的东西，都作为子进程运行，包括钩子、MCP、消息平台和设置程序，通过标准输入输出或 HTTP 通信。所以插件可以用任何语言写，插件崩了也不会把 ACECode 带崩。opencode 的插件是在主进程里运行的 TypeScript 代码，ACECode 不走这条路。

---

## 2. 五分钟上手：一个只有技能的插件

### 目录

```
review-kit/
├── .acecode-plugin/
│   └── plugin.json
├── skills/
│   └── review/
│       └── SKILL.md
└── commands/
    └── review-pr.md
```

### `.acecode-plugin/plugin.json`

```json
{
  "name": "review-kit",
  "version": "0.1.0",
  "description": "按团队约定审查代码变更"
}
```

### `skills/review/SKILL.md`【现有格式】

```markdown
---
name: review
description: 按团队约定审查当前改动，只报告能指出文件位置的问题。
---

先读项目规则和当前差异，逐项检查行为、错误处理和测试。
团队约定见 ${ACECODE_PLUGIN_ROOT}/skills/review/checklist.md。
```

### `commands/review-pr.md`【现有：opencode 命令已经支持 `$ARGUMENTS`】

```markdown
---
description: 审查指定的 PR
---

用 review-kit:review 技能审查 PR $ARGUMENTS，先列问题再给结论。
```

### 开发循环【新增】

```bash
acecode plugin validate ./review-kit
```

```bash
acecode --plugin-dir ./review-kit
```

`--plugin-dir` 只在这一次启动里加载插件，不写安装记录。改完文件后执行 `/skills reload` 就能看到变化。

确认没问题后，给自己装上：

```bash
acecode plugin add ./review-kit
```

装好以后，模型会在合适的时候自动使用 `review-kit:review`；用户也可以输入 `/review-kit:review-pr 123` 手动触发。（今天斜杠命令名会把冒号丢掉，这个写法要先改代码才能成立，见坑 10。）

---

## 3. 目录结构和说明文件

### 3.1 说明文件的查找顺序【新增】

1. `.acecode-plugin/plugin.json`
2. `.claude-plugin/plugin.json`
3. `.codex-plugin/plugin.json`

找到第一份就停，不合并。三份都没有时，只要文件夹里有 `skills/` 这类约定目录，也当作插件处理，名字取文件夹名。

如果想同时发布给 Claude Code 和 ACECode，就放两份：`.claude-plugin/plugin.json` 写标准字段，`.acecode-plugin/plugin.json` 写完整内容（含 ACECode 扩展字段）。Codex 和 Grok 也是这个做法：Codex 查找 `.codex-plugin` → `.claude-plugin` → `.cursor-plugin`，Grok 查找 `plugin.json` → `.grok-plugin` → `.claude-plugin`。

### 3.2 约定目录

| 路径 | 组件 | 来源 |
|---|---|---|
| `skills/<名字>/SKILL.md` | 技能 | Claude 约定 |
| `commands/*.md` | 命令（当作技能处理） | Claude 约定 |
| `hooks/hooks.json` | 钩子 | Claude 约定，格式是 ACECode 现有的 Codex 兼容格式 |
| `.mcp.json` | MCP 服务器 | Claude 约定 |
| `agents/*.md` | 单角色专家 | Claude 约定，ACECode 按假设 4 转换 |
| `experts/<名字>/expert.json` | 专家 | ACECode 扩展 |
| `channels/<名字>/channel-platform.json` | 消息平台 | ACECode 扩展 |
| 其他（`bin/`、`scripts/`、资源文件） | 随意放 | 用变量引用，见第 5 节 |

### 3.3 `plugin.json` 字段

**标准字段**（和 Claude 相同，三份说明文件里都有效）：

| 字段 | 必填 | 说明 |
|---|---|---|
| `name` | 是 | 小写字母、数字、连字符，不超过 64 个字符。它同时是命名空间前缀 |
| `version` | 否 | 建议填写。没填时用来源的 git 提交号或压缩包校验值代替 |
| `description` | 否 | 显示在插件卡片上 |
| `author` / `homepage` / `repository` / `license` / `keywords` | 否 | 元数据 |
| `skills` / `commands` / `agents` / `hooks` / `mcpServers` | 否 | 在约定目录之外**追加**的路径，不是替换。必须以 `./` 开头，不能跳出插件目录 |

**ACECode 扩展字段**（只在 `.acecode-plugin/plugin.json` 里有效）：

| 字段 | 说明 |
|---|---|
| `displayName` | 插件卡片上显示的名字，可以写中文 |
| `icon` | 插件卡片图标，插件内的相对路径 |
| `minAcecodeVersion` | 最低支持的 ACECode 版本，低于它时拒绝安装 |
| `platforms` | 支持的平台，例如 `["windows-x64", "linux-x64", "darwin-arm64"]`，不写表示全平台 |
| `experts` / `channels` | 在约定目录之外追加的路径 |
| `setup` | 设置程序，见 4.7 |

完整例子：

```json
{
  "name": "example-im",
  "displayName": "Example IM",
  "version": "1.4.0",
  "description": "在 Example IM 里收发 ACECode 任务",
  "icon": "./icon.svg",
  "minAcecodeVersion": "1.1.0",
  "platforms": ["windows-x64", "linux-x64"],
  "setup": {
    "command": {
      "windows-x64": "./bin/windows-x64/example-auth.exe",
      "linux-x64": "./bin/linux-x64/example-auth"
    },
    "args": ["--data", "${ACECODE_PLUGIN_DATA}"],
    "timeout_ms": 300000
  }
}
```

---

## 4. 各组件的写法

### 4.1 技能【现有格式，新增前缀与变量】

格式和今天完全一样。frontmatter 支持 `name`、`description`、`when_to_use`、`platforms`、`tags`（`src/domain/skills/skill_metadata.hpp`）。

放进插件后有两处不同：

- **名字自动加前缀**：变成 `插件名:技能名`，不会和用户自己的同名技能冲突。用户自己的技能不加前缀。
- **正文里可以写 `${ACECODE_PLUGIN_ROOT}`**：加载时替换成插件目录的绝对路径。今天没有这个变量，Understand Anything 的适配版只好在每份 SKILL.md 里写「把 PACKAGE 替换为本 SKILL.md 向上两层的绝对路径」，让模型自己去算路径。

### 4.2 命令【现有逻辑，新增入口】

`commands/*.md` 加载成技能，用户输入 `/插件名:命令名 参数` 调用。正文里的 `$ARGUMENTS` 会替换成参数，复用 opencode 命令现有的实现（`src/domain/skills/opencode_command.cpp`）。frontmatter 必须有 `description`。

注意：今天**技能**正文里的 `$ARGUMENTS` 不会被替换，只有 opencode 命令会。Claude 插件里有不少技能正文直接写了 `$ARGUMENTS`。

### 4.3 钩子【现有格式，新增变量与信任方式】

格式就是今天的 Codex 兼容格式，完整说明见 `docs/hooks.md`。

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {
            "type": "command",
            "command": "python \"${ACECODE_PLUGIN_ROOT}/hooks/after_bash.py\"",
            "timeout": 10
          }
        ]
      }
    ]
  }
}
```

**事件**【现有】，实际会触发的有 10 种：`SessionStart`、`SessionTitleChanged`、`UserPromptSubmit`、`PreToolUse`、`PermissionRequest`、`PermissionResolved`、`PostToolUse`、`PreCompact`、`PostCompact`、`Stop`。其中 `SessionTitleChanged` 和 `PermissionResolved` 是 ACECode 独有的。另外 `SubagentStart`、`SubagentStop` 只会被解析，代码里没有任何触发点（`src/domain/hooks/hook_runtime.hpp` 的事件常量里没有这两个），挂在上面的钩子永远不会运行。

**写插件钩子时要知道的现状**【现有】：

- 只支持 `command` 类型。`prompt`、`agent`、`http` 类型和 `async: true` 会被跳过并给出诊断。
- matcher 里写 `Bash`、`Edit`、`Write`、`apply_patch` 会映射到原生工具，其他名字按原样匹配（`src/domain/hooks/hook_runtime.cpp` 的 `canonical_hook_match_value`）。
- **输入 JSON 里的 `tool_name` 是 ACECode 原生名**，比如 `bash`、`file_edit`，不是 `Bash`。脚本里写 `if tool_name == "Bash"` 会判断失败。
- 钩子的工作目录是会话目录。今天环境变量只有一个 `ACECODE_HOOK_SESSION_TITLE`。
- 纯文本 stdout 或 `hookSpecificOutput.additionalContext` 会作为上下文交给下一次模型请求。

**插件钩子新增的部分**【新增】：

- 环境变量 `ACECODE_PLUGIN_ROOT`、`ACECODE_PLUGIN_DATA`，同时提供别名 `CLAUDE_PLUGIN_ROOT`、`CLAUDE_PLUGIN_DATA`。命令字符串里的 `${...}` 也会被替换。
- 信任：安装插件时确认一次，之后按内容哈希记下信任。升级后如果钩子内容变了，需要重新确认。

**建议**：钩子写成 Python 或 Node 脚本，不要写成 shell 单行命令。shell 单行命令在 Windows 上大多跑不起来；实在要写，就用 `commandWindows` 单独给 Windows 写一份【现有】。

### 4.4 MCP 服务器【现有格式，新增转换】

ACECode 自己的写法【现有】，和项目级 `.acecode/mcp.json` 相同（`src/base/config/mcp_config.cpp`）：

```json
{
  "mcp_servers": {
    "tickets": {
      "transport": "stdio",
      "command": "node",
      "args": ["${ACECODE_PLUGIN_ROOT}/mcp/tickets.mjs"],
      "env": { "TICKETS_DATA": "${ACECODE_PLUGIN_DATA}" }
    }
  }
}
```

插件里的 `.mcp.json` 也接受 Claude 的写法【新增】，ACECode 加载时负责转换：

- `{"mcpServers": {...}}` 包一层的写法，和服务器名直接放在顶层的写法都认。
- `type` 转成 `transport`。

规则【新增】：

- 用户在 `config.json` 或项目 `.acecode/mcp.json` 里配了同名服务器时，**用户的配置优先**，插件里的这一个被跳过并给出诊断。
- 服务器名加不加插件前缀还没定，见坑 10。

### 4.5 专家【现有格式】

`experts/<id>/expert.json` 加 `agents/*.md`，格式和今天 `~/.acecode/experts/` 下的完全一样（`src/domain/experts/expert_registry.cpp`）。`expert.json` 本来就能引用 `skills`、`tools`、`mcp_servers`，引用同一插件里的技能时写带前缀的名字。

Claude 插件的 `agents/*.md`【新增，按假设 4】会读成单角色专家：

- `name` 和 `description` 直接对应。
- `tools` 对应工具白名单。
- `model` 先忽略。
- `permissionMode`、`hooks`、`mcpServers` 一律忽略：插件里的角色不能放宽权限，Claude Code 和 Grok 也是这么处理的。

### 4.6 消息平台【现有协议，新增位置和多平台】

协议完全不变，见 `docs/channel-platform-plugin.md`。变化在于清单的位置和几个字段：

- 清单放在插件里的 `channels/<名字>/channel-platform.json`，不再放 `<data_dir>/connectors/` 下。旧目录继续扫描。
- `command` 可以写成按平台区分的对象【新增】，写法同 3.3 的 `setup.command`。
- `${manifest_dir}` 继续可用【现有，`src/host/channels/core/plugin_platforms.cpp`】，另外新增 `${ACECODE_PLUGIN_ROOT}`、`${ACECODE_PLUGIN_DATA}`。
- 子进程的 stderr 写进 daemon 日志，带截断和限速【新增】。今天是直接丢弃。
- 启用、停用插件时，消息平台子进程随之启停，不需要重启 daemon【新增】。今天清单只在 daemon 启动时扫描一次。

### 4.7 设置程序【新增，取代 config.json 的 `connectors`】

用于「装好后要先登录、换取密钥」这类集成。它是一个一次性子进程，在 `plugin.json` 的 `setup` 字段里声明（见 3.3）。

- **何时运行**：插件第一次启用时运行，此时用户在场，设置页显示进度。插件卡片上的「重新连接」可以再运行一次。
- **结束约定**：退出码 0 表示完成。stdout 的最后一行如果是 `{"status":"error","message":"..."}`，就把 message 显示给用户。

今天的状况【现有】：`config.json` 的 `connectors[].hooks.on_startup` 只在这台机器第一次启动 daemon 时运行一次。`on_enable`、`on_auth_error` 虽然还在解析，但运行时不会执行（`src/base/config/config.cpp` 的 `startup_hook_connectors`）。也就是说，令牌过期后，今天没有任何重新认证的入口。

---

## 5. 两个目录：ROOT 和 DATA【新增】

| 变量 | 指向 | 升级时 |
|---|---|---|
| `ACECODE_PLUGIN_ROOT` | `<data_dir>/plugins/cache/<名字>/<版本>/` | 换成新目录 |
| `ACECODE_PLUGIN_DATA` | `<data_dir>/plugins/data/<名字>/` | 保留 |

**规则：永远不要往 ROOT 里写东西。** 登录状态、消息库、缓存都写进 DATA。

为什么 ROOT 要按版本分目录：Windows 上正在运行的 exe 无法被覆盖，本仓库刚处理过升级时报「文件被占用」（错误 32）的问题。把新版本装进新目录，切换过去后重启子进程，旧目录等没有进程使用时再清理，就绕开了这个问题。Claude Code 也是这样做的：旧版本目录先打标记，7 天后清理。

变量会在这些位置被替换：

- 技能和命令的正文
- 钩子命令
- `.mcp.json` 里的 `command`、`args`、`env`
- 消息平台清单里的 `command`、`args`
- `setup`

别名 `CLAUDE_PLUGIN_ROOT`、`CLAUDE_PLUGIN_DATA` 在上述所有位置同样有效。

---

## 6. 安装、启停、升级、卸载【新增】

| 操作 | 命令行 | TUI | 设置页 |
|---|---|---|---|
| 安装 | `acecode plugin add <文件夹 \| zip \| 网址> [--sha256 <值>]` | `/plugins add` | 集成 > 插件 >「添加插件」 |
| 列表 | `acecode plugin list` | `/plugins` | 两列卡片 |
| 详情 | `acecode plugin show <名字>` | `/plugins show <名字>` | 点开卡片 |
| 启用 / 停用 | `acecode plugin enable \| disable <名字>` | 同左 | 卡片开关 |
| 升级 | `acecode plugin update [<名字>]` | 同左 | 卡片菜单 |
| 卸载 | `acecode plugin remove <名字> [--purge]` | 同左 | 卡片菜单 |
| 检查 | `acecode plugin validate <文件夹>` | — | — |

- 卸载默认保留 DATA 目录，加 `--purge` 才一起删除。
- 来自插件的技能、钩子、MCP 服务器、专家，在各自的设置页里会标出所属插件。它们只能随插件一起停用，不能单独删除。
- 一期只做用户级插件。项目级插件（放在 `.acecode/plugins/` 下、跟随工作区信任）放到三期。

**改动何时生效**：

| 组件 | 生效时机 |
|---|---|
| 技能、命令 | 立即生效（技能已经支持不重启刷新）【现有】 |
| 钩子 | 立即生效（重新加载钩子注册表） |
| MCP | 立即生效（重新加载 MCP 运行时） |
| 专家 | 立即生效 |
| 消息平台 | 立即生效【新增，今天需要重启 daemon】 |

**多个进程同时运行时**：桌面版每个工作区有一个 daemon，另外还可能开着 TUI 和 headless。它们共用 `<data_dir>/plugins/installed.json` 这一份安装记录，各进程发现这个文件的修改时间变化后重新加载，类似今天模型配置的做法（手改 `config.json` 里的模型配置会即时生效）。

**headless**：沿用今天的规则，默认不启用 MCP，技能需要显式开启。插件组件用带前缀的名字放进 `--enable-skills` / `--enable-mcp`。另外新增 `--enable-plugins <名字>`，一次开启某个插件的全部组件。

---

## 7. 信任

安装确认页列出三类信息，用户确认等于信任**这一个版本**（按内容哈希记录）：

- **会运行的程序**：钩子命令、MCP 的启动命令、消息平台程序、设置程序。
- **会访问的网址**：HTTP 类型的 MCP 地址。
- **其他组件的数量**：技能、命令、专家各有几个。

升级时，如果会运行的程序有变化，需要再确认一次，确认页只显示变化的部分。

其他规则：

- 插件里的路径不能跳出插件目录。
- 插件里的专家不能放宽权限（见 4.5）。
- 企业环境可以设置允许的来源白名单，放到三期。

---

## 8. 分发【新增】

**本地文件夹或 zip**

```bash
acecode plugin add ./example-im-1.4.0.zip
```

**网址加校验值**：适合无法访问 GitHub 的环境。自建的下载服务器通常本来就是「zip + SHA256SUMS」的发布方式。

```bash
acecode plugin add https://intranet.example/aupdate/example-im-1.4.0.zip --sha256 3f9a...
```

**市场**：一个 `marketplace.json` 文件，格式和 Claude 相同，放在 git 仓库、本地目录或某个网址上。ACECode 额外支持 `zip` 这种来源类型：

```json
{
  "name": "team-plugins",
  "owner": { "name": "Team" },
  "plugins": [
    { "name": "review-kit", "source": "./review-kit", "description": "按团队约定审查代码" },
    {
      "name": "example-im",
      "source": {
        "source": "zip",
        "url": "https://intranet.example/aupdate/example-im-1.4.0.zip",
        "sha256": "3f9a..."
      }
    }
  ]
}
```

```bash
acecode plugin marketplace add https://intranet.example/aupdate/marketplace.json
```

```bash
acecode plugin add example-im@team-plugins
```

---

## 9. 调试

- `acecode plugin validate <文件夹>`【新增】：检查字段、路径、命名，并列出哪些组件 ACECode 不支持、会被忽略。
- `acecode plugin show <名字>`【新增】：显示每个组件的加载结果和诊断信息。
- daemon 日志里的 `[plugins]` 行【新增】。消息平台子进程的 stderr 也会写进日志。
- 钩子诊断看「设置 > 钩子」【现有】。

---

## 10. 拿现成的 Claude / Codex 插件来用：能用多少

**这里更正上一轮的说法。** 上一轮我说「现成的 Claude/Codex 插件放进来就能用」，说过头了。准确的说法是：**打包和安装可以兼容，内容不一定兼容。**

证据是仓库里已经做过的一次真实移植：`packaging/understand-anything/`。它把 Understand Anything（一个带技能、角色和钩子的 Claude Code 插件，钉在上游 v2.9.0）适配成 ACECode 的 9 个命令。这次提交一共约 1000 行：

- 打包脚本 `packaging/understand-anything/build.py`，除了组装，还用 `replace_once` 给上游的 `.mjs` / `.py` 源码打了好几处补丁；
- 新写的编排脚本 `ua.py`、`refresh.py`、`special.py`；
- 冒烟测试 `smoke.py`；
- 9 份重写过的 SKILL.md。

下表列出当时不得不改的地方，以及方案落地后的情况：

| 上游写法 | 今天的 ACECode | 方案落地后 |
|---|---|---|
| SKILL.md 和钩子里写 `${CLAUDE_PLUGIN_ROOT}` | 没有这个变量。适配版改成「本 SKILL.md 向上两层」 | 别名替换，原样可用 |
| 技能正文写 `$ARGUMENTS` | 技能正文不替换，只有 opencode 命令替换 | 要决定技能正文是否也替换（见坑 1） |
| 「用 `agents/x.md` 派一个子代理」 | 靠模型自己对应到 `spawn_subagent`。适配版把工具名写死了 | `agents/` 读成专家后可以按名字派遣，但模型能否照做仍不保证 |
| 钩子写成 POSIX shell 单行命令，读取 `$TOOL_INPUT` 环境变量 | Windows 上跑不起来。ACECode 不提供 `$TOOL_INPUT`，输入在 stdin 的 JSON 里 | **仍然需要作者修改**，`validate` 给出提示 |
| 钩子脚本判断 `tool_name == "Bash"` | 输入里是 `bash` | 要决定是否给 Claude 来源的插件发 Claude 风格的名字（见坑 2） |
| 运行时依赖 pnpm 工作区、`node_modules`、自行构建 | 适配版把运行时整包打了进去 | 插件机制不负责安装依赖，作者要把运行时放进包里，或者写明依赖 |
| 编排流程写在技能正文里：分阶段派遣子代理、最多 5 个并发、读中间文件 | 整套流程换成了适配版自己写的 `ua.py`，技能正文只剩调用步骤 | **插件机制解决不了。** 这是「宿主行为不同」，不是「包格式不同」 |
| 上游脚本假定 POSIX 路径和自己的数据目录 | 打包时用 `replace_once` 给上游源码打补丁 | **插件机制解决不了。** 只能由作者改，或由移植者打补丁 |

归纳成四类：

| 类别 | 内容 |
|---|---|
| 原样可用 | 纯 Markdown 技能、命令（`$ARGUMENTS`）、`.mcp.json`、用 Python 或 Node 写并且读 stdin 的钩子 |
| 自动转换 | `${CLAUDE_PLUGIN_ROOT}` 等变量、Claude 写法的 `.mcp.json`、`agents/*.md` |
| 忽略并提示 | 输出风格（output styles）、LSP 配置、`prompt` / `agent` / `http` 类型的钩子、ACECode 没有或不触发的钩子事件（包括 `SubagentStart` / `SubagentStop`） |
| 需要作者修改 | shell 单行钩子、依赖 Claude 专有环境变量或工具名的脚本、写死「Task 工具」用法的技能正文 |

---

## 11. 例子：消息平台加首次登录，改造前后对比

**改造前**（示意，按今天的机制推出来，用中性名字）：

```
<data_dir>/connectors/example-im/        消息平台程序和清单
<data_dir>/local-hooks/example-auth/     换取密钥的程序。ACECode 不扫描这个目录
config.json → connectors[]               设置页「插件」里的那一行，on_startup 指向上面的绝对路径
```

安装器要往三个地方写东西，还要改 `config.json`。卸载的人得去三个地方找。也没有版本号，没法升级。

**改造后**：

```
example-im/
├── .acecode-plugin/plugin.json          内容见 3.3 的完整例子
├── icon.svg
├── channels/example-im/channel-platform.json
├── bin/windows-x64/example-im.exe
├── bin/windows-x64/example-auth.exe
├── bin/linux-x64/example-im
├── bin/linux-x64/example-auth
└── skills/example-im-help/SKILL.md
```

`channels/example-im/channel-platform.json`：

```json
{
  "schema": "acecode.channel-platform.v1",
  "name": "example-im",
  "label": "Example IM",
  "command": {
    "windows-x64": "../../bin/windows-x64/example-im.exe",
    "linux-x64": "../../bin/linux-x64/example-im"
  },
  "args": ["--channel-transport", "--data", "${ACECODE_PLUGIN_DATA}"],
  "account": "self",
  "features": ["watched_groups"]
}
```

安装器只做一件事：

```bash
acecode plugin add example-im-1.4.0.zip --sha256 3f9a...
```

卸载：

```bash
acecode plugin remove example-im
```

---

## 12. 写这份说明书时发现的坑

这些是以开发者身份把说明书写一遍才冒出来的问题。每条都需要在方案里给出答案。

1. **包格式兼容不等于内容兼容。** 见第 10 节。还要决定一件事：技能正文里的 `$ARGUMENTS` 要不要也替换。不替换的话，大量 Claude 技能拿到的参数是空的。
2. **钩子输入里的工具名。** ACECode 发的是原生名（`bash`），Claude 的脚本按 `Bash` 判断。可选做法：对来源是 `.claude-plugin` 的插件，在钩子输入里改发 Claude 名；或者不改，由 `validate` 提示作者。前者更兼容，但同一个事件在不同插件里看到的字段不一样。
3. **消息平台子进程的工作目录是清单所在目录。** 现有插件很可能把登录状态写在那里。改成按版本分目录安装后，升级就会丢登录。需要约定改写到 DATA 目录，并且给旧插件一次性迁移的机会。
4. **Windows 上运行中的 exe 不能替换。** 用按版本分目录解决（第 5 节），但升级流程必须是「装进新目录 → 停旧进程 → 用新目录启动」，不能原地覆盖。
5. **消息平台的 stderr 被丢弃，插件作者没法调试。** 改成写进 daemon 日志，同时要防止刷屏。
6. **多平台二进制。** 技能有 `platforms`，钩子有 `commandWindows`，但消息平台清单只有一个 `command`。另外 Android 版是 proot 里的 Linux，平台应为 `linux-arm64`，插件作者容易漏掉。
7. **没有重新认证的入口。** 今天认证只在首次启动 daemon 时跑一次。令牌过期以后怎么办，需要「重新连接」按钮，或者让消息平台通过事件报告「需要重新登录」。
8. **多个进程之间的同步。** 桌面版的多个 daemon、TUI、headless 共用一份安装记录。一个进程装了插件，其他进程靠修改时间感知。消息平台由哪个 daemon 负责运行，今天就已经是「每个 daemon 各自扫描」，要确认同一个平台不会被启动两次。
9. **headless 的开关。** `--enable-plugins` 是新加的。插件里的钩子在 headless 下默认运不运行，需要决定。今天 headless 下钩子照常运行，MCP 默认关闭。
10. **命名空间分隔符，今天就过不去。** 技能的斜杠命令名由 `normalize_skill_command_key` 生成（`src/domain/skills/skill_loader.cpp`），只保留字母、数字和连字符，其他字符静默丢掉。所以名字叫 `review-kit:review` 的技能，命令会变成 `/review-kitreview`。要用冒号做命名空间，得先放开这个函数，再逐个核对 TUI 的斜杠命令解析、Web 的命令列表和输入框补全、headless 的 `--enable-skills` 名称匹配（这一项还没查）。MCP 那边更严格：工具名会发给模型，OpenAI 只允许字母、数字、下划线和连字符，最长 64 个字符。所以 MCP 服务器名需要另一套前缀规则，比如 `<插件名>-<服务名>`，过长时截断再加哈希。
11. **两套渠道协议并存。** `/rc` 渠道插件（`acecode.channel-plugin.v1`，在 `config.json` 里写清单路径）和消息平台插件（`acecode.channel-platform.v1`，放进 `<data_dir>/connectors/` 由 daemon 扫描），都是「外部程序接入一个聊天渠道」，集成方很容易选错。按假设 3 收敛到消息平台协议，并给 `/rc` 留兼容。
12. **「插件」这个名字要让出来。** 设置页现在叫「插件」的是 `connectors` 列表。这一页要么改名为「旧式集成」并入新的插件页，要么在迁移完成后删掉。
13. **插件钩子需要新的来源类型。** 今天受托管的种子钩子靠代码里写死的 SHA 来自动信任（`src/domain/hooks/hook_seeds.cpp`）。插件钩子既不是用户手写的，也不是种子，需要一种新的来源和信任记录。
14. **内置种子可以统一成一个内置插件。** 今天种子技能、`agent-reporting` 钩子、预置专家各有各的种子机制。将来可以统一成一个随程序发布的内置插件，不放进一期。

## 13. 待拍板

- 第 0 节的四条假设。
- 有多份说明文件时，只读第一份，还是合并读取？
- 启用状态存在 `installed.json`，还是存进 `config.json`？
- 设置程序在什么时候运行（第一次启用时 / 每次启动时 / 只靠手动「重新连接」）？
- 坑 1：技能正文里的 `$ARGUMENTS` 是否替换。
- 坑 2：Claude 来源插件的钩子输入里，工具名是否改发 Claude 名。
- 坑 9：headless 下插件钩子默认是否运行。
- 坑 10：命名空间分隔符用冒号（和 Claude、Codex、Grok 一致，但要改命令名生成和各处解析），还是换成 ACECode 今天就能通过的字符，比如两个连字符。
