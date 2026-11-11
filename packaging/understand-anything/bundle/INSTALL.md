# Understand Anything for ACECode

上游 v2.9.0 · ACECode 适配版 1

## 安装

将 ZIP 中的整个 `understand-anything` 文件夹解压到 ACECode 的技能扫描目录。默认用户目录为 `~/.acecode/skills/`，也可以放在项目的 `.acecode/skills/`。使用自定义数据目录时，在 ACECode 的技能设置中查看实际扫描路径。解压后应存在 `understand-anything/skills/understand/SKILL.md`。共享脚本、node_modules、viewer 与九个技能须放在同一个包内。已有同名技能时，请检查技能列表中的实际来源。

在 ACECode 运行 `/skills reload` 或重新打开会话。运行环境需要 Node.js 22+ 和 Python 3.11+。代码分析使用当前 ACECode 会话的模型。Git 支持版本识别和差异分析。解析器、语言语法和本地 Viewer 已包含在包中。

## 使用

```text
/understand --language zh
/understand-chat 应用从哪里启动？
/understand-explain src/apps/cli/main.cpp
/understand-diff
/understand-onboard
/understand-dashboard
```

`/understand-domain` 生成业务流程图，`/understand-knowledge <Wiki目录>` 分析 Markdown Wiki，`/understand-figma <URL>` 分析 Figma 设计。Figma 需要环境变量 `FIGMA_TOKEN` 和网络。

新项目保存到 `.ua/`，已有 `.understand-anything/` 的项目沿用该目录。再次运行 `/understand` 会比较文件内容。首次读取旧版图谱可直接问答或查看，首次用适配版刷新时会重新分析。大仓库可传子目录或配置数据目录内的 `.understandignore` 控制范围。

`/understand --auto-update` 为当前项目登记 ACECode 钩子，按 ACECode 的提示审核并重新加载后生效。`--no-auto-update` 取消。搬动安装位置后重新开启一次。如果进程在发布期间被强制中断，下次发布会报告已有锁。确认对应 PID 已结束后可移除数据目录内的 `acecode-publish.lock` 并重试。

上游源码版本、Viewer 校验值、依赖版本记录在 `UPSTREAM.json`。授权文本见 `LICENSE`、`licenses/` 和各运行时组件目录。
