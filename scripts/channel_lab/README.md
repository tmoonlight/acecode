# 本机 IM / Channel 测试

一个只依赖 Python 3.10+ 标准库的临时文本 IM。左侧“自己”会话由服务端原样回送；“ACECode”会话使用真实 Channel v1 插件，经 `/rc/send` 入站、webhook 出站。无需 IM 账号。

## 启动

从仓库根目录运行（Windows，复用本检出的已有原生构建）：

```powershell
python scripts/channel_lab/server.py --acecode build/Release/acecode.exe --open
```

也可以双击 `scripts/channel_lab/start.bat`。首次自动选择空闲 IM 端口，控制台打印地址并打开浏览器。

重复启动同一运行目录时，会直接显示已运行实例的完整访问 URL；使用该地址即可，不会重复创建服务。

需要固定端口或选择已保存模型时：

```powershell
python scripts/channel_lab/server.py --acecode build/Release/acecode.exe --port 39123 --model-name "你的已保存模型名" --open
```

`--model-name` 仅影响新建测试会话；已有会话继续使用其持久化模型。省略 `--acecode` 可仅运行自聊服务。通用协议见 `docs/channel-plugin-protocol.md`。完整自动接入启动目前支持 Windows。

## 使用

- **帮助**：在任一会话输入 `/help` 或点击右上角“帮助”，直接列出会话导航、问题回答等指令和示例。帮助由 IM 本地提供，Channel 未连接也可查看，记录随历史保存。
- **自己**：发送中文、多行文本；服务端保存并原样回送。Enter 发送，Shift + Enter 换行，中文输入法选字不触发发送。
- **ACECode**：直接聊天，或使用“会话列表”“全部会话”“待回答问题”“取消提问”。输入 `/session <编号>` 切换测试会话，`/session search <关键词>` 搜索，`/aq <回答>` 回答问题。
- **断开连接 / 连接 ACECode**：执行测试 daemon 的 `/rc off` / `/rc` 生命周期。
- **历史**：刷新、重开窗口和服务重启后保留；多个浏览器窗口每秒同步，单页最近 200 条，可加载更早消息或导出全部 JSON。
- **发送状态**：已发送表示服务端/Channel 接受，不代表模型已完成回答。HTTP 拒绝标记失败；超时等未知结果不会自动重发，可以把文本放回输入框后自行判断。模型服务不可用时仍可测试 `/session` 和 `/aq --status`。

精简范围为单人、本机、文字会话；不提供第三方账号、附件、音视频。快捷按钮只使用通用 Channel 实际支持的命令，不能将 WhatsApp 专属的 `/stop`、`/approve` 等当成通用协议命令。

## 停止及运行数据

点击页面左下角“停止测试服务”，或运行：

```powershell
python scripts/channel_lab/server.py --stop
```

运行数据默认在 `%TEMP%/acecode-channel-lab/`，可通过 `--runtime-dir <目录>` 隔离多套测试。停止命令要传入相同的 `--runtime-dir`。

- `history.sqlite3`：IM 历史，不记录凭据。
- `profile/.acecode/`：子进程专用测试配置和测试会话；仅复制模型相关设置，不继承生产 MCP、Channel 绑定和会话。
- `plugin-runtime.json`、`server.json`：含本地管理凭据，不能分享或提交。
- `channel-plugin.json`：通用插件 manifest；短进程 `plugin.py` 连接当前 IM 服务。
- `daemon.log`：原生测试进程日志，排障时注意其中可能含本地路径和模型请求信息。

工具仅监听 `127.0.0.1`。它不会覆盖正常用户配置或停止现有 Desktop/daemon。退出时仅停止它自己启动的测试 daemon，历史保留。全新测试可选择一个新的 `--runtime-dir`。

## 验证

```powershell
python -m unittest discover -s scripts/channel_lab -p test_server.py -v
```

可选浏览器验收需要已有 Python Playwright 和 Edge；对运行中的测试实例执行（会添加测试消息并切换测试会话）：

```powershell
python scripts/channel_lab/smoke_browser.py --url http://127.0.0.1:39123 --output "$env:TEMP/channel-lab-screenshots"
```

静态页面位于本工具的 `web/` 子目录，直接由 Python 提供，不依赖正式 WebUI 的打包产物。
