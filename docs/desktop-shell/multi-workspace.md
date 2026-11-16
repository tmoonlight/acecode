# Desktop multi-workspace 备忘

> 配套 change: `openspec/changes/add-desktop-multi-workspace/`、`openspec/changes/share-daemon-across-workspaces/`

## 关键模型

```
acecode-desktop.exe / ACECode.app (父)
  ├── WebHost (webview, 主窗口)
  ├── WorkspaceRegistry  ← .acecode/projects/<hash>/workspace.json
  └── DaemonPool
    └── Slot[__shared_daemon__/default]
      └── DaemonSupervisor → acecode.exe / Contents/MacOS/acecode-daemon daemon (shared port/token)
```

所有 workspace / session / conversation 共享同一个 daemon 子进程。workspace
身份通过 HTTP API 和 `SessionOptions{cwd, workspace_hash}` 传入 session 层,
每个 session 的 `AgentLoop`、`SessionManager`、tool context 都使用自身 cwd。
磁盘布局仍然是 `.acecode/projects/<cwd_hash>/`,兼容旧 session。

## 启动顺序

1. desktop 扫 `.acecode/projects/*` → `WorkspaceRegistry`
2. 读 `state.json::last_active_workspace_hash`;没有显式打开请求且 `state.json::last_session` 可恢复时,它所在的工作区优先(见「回到上次会话」)
3. `pick_active(last_active, current_path, registry)` → active hash
4. registry 空 + 进程 cwd 非空 → 自动 `register_new(cwd)`(首次安装兜底)
5. 注册 4 个 webview bridge:`aceDesktop_listWorkspaces / activateWorkspace / renameWorkspace / addWorkspace`
6. `pool.activate(__shared_daemon__/default)` → 拿到 `{port, token}` → `host.navigate("http://127.0.0.1:port/?token=...")`
7. 主窗口 `host.run()`

## 回到上次会话

启动时直接打开上次停留的会话,实现在 [last_session.cpp](../../src/apps/desktop/last_session.cpp) 与 [lastSessionMemory.js](../../web/src/lib/lastSessionMemory.js):

- **记**:前端在当前会话变化时调 `aceDesktop_setLastSession({session_id, workspace_hash?, no_workspace?, read_only?})`,停在首页 / 定时任务 / 专家组件页时传 `null`,桌面壳写进 `state.json::last_session`(内容没变不落盘)。启动跳转落定之前不上报 —— 那时 `activeRef` 还是空的,会把上次的记录清掉。
- **用**:启动时 `restorable_last_session(registry)` 读回记录(带工作区的会话要求该工作区仍在 registry 且目录还在),该工作区优先成为 active,导航前给 URL 拼上 `?open=<会话>&workspace=<hash>`(或 `no_workspace=1`)`&restore=1`,走与跨 workspace 跳转相同的启动落地。`restore=1` 让前端静默处理失败:会话已被删除时回到首页,不弹「恢复失败」。
- TUI 的 `--open-session` / 邀请链接等显式打开请求优先,此时不恢复。
- 记录只由桌面壳读写:端口每次启动都变,浏览器本地存储留不住;浏览器直连 daemon 与 webapp 兼容模式没有 bridge,不记也不恢复。

## 升级后重启

`aceDesktop_restartApp` 只做预检并请求退出;真正拉起新进程在 `host.run()` 返回、托盘与 daemon 全部收尾之后([desktop_restart.cpp](../../src/apps/desktop/desktop_restart.cpp))。那一刻旧进程的窗口早已销毁,它不再是前台程序,新进程因此拿不到前台权 —— 窗口会出现在别的窗口后面。处理分两头:

- **旧进程**:拉起时带 `--relaunched-after-update`。Windows 上顺手 `AllowSetForegroundWindow(新进程)`(只在自己仍有前台权时生效);macOS 上已安装的 `.app` 改经 LaunchServices 重启(`/usr/bin/open -n <bundle> --args …`),由系统激活新实例,`open` 失败、裸二进制(开发构建)或进程带着 `ACECODE_*` 环境变量(LaunchServices 不继承)时仍直接 exec。
- **新进程**:带该参数、或这是本版本装好后的第一次启动(`state.json::desktop_last_launch_version` 与当前版本不同;旧版本拉起新版本时不会带参数,靠这条兜住)时,首次显示主窗口走 [window_foreground.cpp](../../src/apps/desktop/window_foreground.cpp):临时并入当前前台线程的输入队列再激活(前台程序无响应时不并入),仍被拒绝就用一次置顶脉冲把窗口抬到最上层后立刻撤销。平时的启动不走这条路,用户启动后切去别的程序时不抢焦点。

macOS 的修复由旧进程执行,所以要到「装了这一版之后的下一次升级」才生效。

## 切换 workspace

前端 Sidebar 优先调用共享 daemon:
1. `GET /api/workspaces` 拉 workspace 列表。
2. `GET /api/workspaces/:hash/sessions` 按 workspace 拉 session。
3. 点击 workspace 只更新前端 active workspace,不切端口、不派生新 daemon。
4. 恢复 inactive conversation 时调用 `POST /api/workspaces/:hash/sessions/:id/resume`。

`aceDesktop_activateWorkspace(hash)` 仍保留为 bridge fallback:它只确保共享
daemon slot 已启动,并向 daemon `POST /api/workspaces` 注册 cwd,返回同一组
`{port, token}`。

## 添加 workspace

`+` 按钮 → `aceDesktop_addWorkspace()`:
1. desktop 端 `pick_folder` 弹 `IFileOpenDialog`(`FOS_PICKFOLDERS`)
2. 用户选目录 → `registry.register_new(cwd)` 写 `workspace.json`
3. 返回 `{hash, cwd, name}`
4. sidebar 调共享 daemon `POST /api/workspaces` 注册 cwd,然后把该 hash 设为 active

## 行内重命名

铅笔按钮 → input 替换 name 文本 → Enter / blur 提交 → `aceDesktop_renameWorkspace(hash, name)` → `set_name` 原子写盘 → sidebar 显示新名。Esc 取消复原。空名 / 未知 hash / 写盘失败均拒绝。

## 退出

- WebView 窗口关闭 → `host.run()` 返回
- 写 `last_active_workspace_hash` 到 `state.json`
- `pool.stop_all()` 停掉共享 daemon slot
- `Job Object KILL_ON_JOB_CLOSE` 是兜底:即使 desktop 进程被外部 kill,daemon 也会跟着死(参见 `add-desktop-shell` 的 design)

## 打开文件夹 / 在资源管理器中显示

`aceDesktop_openInExplorer([path])` 是异步 bridge(`WebHost::bind_async`):[open_in_explorer_bridge.cpp](../../src/apps/desktop/open_in_explorer_bridge.cpp) 把调用放到后台线程,窗口出来后才回复,WebView UI 线程不被占用。Windows 实现在 [open_in_explorer.cpp](../../src/base/platform/native_ui/open_in_explorer.cpp):

- 文件走 `SHOpenFolderAndSelectItems`(Chromium / Electron 的 showItemInFolder 同款),直接交给正在运行的资源管理器,文件夹已开着就复用那个窗口。**不要改回 `explorer.exe /select`**:它会让 COM 另起一个 `explorer.exe /factory,{75dff2b7…} -Embedding` 宿主进程来放窗口,实测约 1 秒,窗口关掉后宿主进程也不退出;现在只在 Shell 接口失败时兜底用它。
- 目录走 `ShellExecuteEx`(`SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI`)。
- Shell 接口要求 STA,调用方线程的 COM 状态不确定,所以每次起一个干净的 STA 线程执行。
- 前台:打开前 `AllowSetForegroundWindow(ASFW_ANY)`(窗口可能落在任意一个 explorer.exe,拿不到 PID);窗口出现后用 `IShellWindows::FindWindowSW` 找到它,前台还停在发起方就拉起来,用户已经切到别的程序就不抢(`decide_file_manager_window_action`)。没有前台权时(daemon / TUI)用 TOPMOST 脉冲抬 z 序并立即撤销;已经被用户设成置顶的窗口不碰,免得撤掉用户的置顶。

## 关键文件

邀请桥接由 [invite_controller.cpp](../../src/apps/desktop/invite_controller.cpp) 管理：

- `aceDesktop_getInviteClipboardPreference()` 读取当前原生识别开关。
- `aceDesktop_applyInviteClipboardPreference(enabled)` 应用已由 daemon 保存的开关，不另写配置文件。
- `aceDesktop_markInviteSeen(link)` 抑制应用自己复制的邀请。
- `acecode:open-invite` 事件携带 `{link}`；页面未就绪时在内存中排队。
- `--open-invite` 与工作区/会话打开请求互斥，第二实例交接后退出。


| 路径 | 作用 |
|---|---|
| `src/base/workspace/workspace_registry.{hpp,cpp}` | 扫盘 / 读写 `workspace.json` / 默认命名 |
| `src/apps/desktop/daemon_pool.{hpp,cpp}` | 通用进程池;Desktop 当前只使用 `__shared_daemon__/default` slot |
| `src/apps/desktop/daemon_supervisor.{hpp,cpp}` | 单 daemon 子进程托管(spawn / probe / stop / Job Object)。提了 `IDaemonSupervisor` 虚基类便于单测 mock |
| `src/base/platform/native_ui/folder_picker_win.cpp` | `IFileOpenDialog` 包装 |
| `src/apps/desktop/pick_active.{hpp,cpp}` | 启动时挑哪个 workspace 当 active 的纯函数 |
| `src/apps/desktop/web_host.{hpp,cpp}` | webview/webview wrapper,暴露 `bind` / `eval` / `native_window` |
| `src/apps/desktop/main.cpp` | wWinMain 入口,串起所有 |
| `src/base/utils/cwd_hash.{hpp,cpp}` | desktop 与 SessionStorage 共享的 hash 算法(FNV-1a 64bit) |
| `src/host/session_host/session_registry.{hpp,cpp}` | workspace-aware session create/resume/list |
| `src/apps/web/server.{hpp,cpp}` | `/api/workspaces` 与 workspace-scoped session routes |
| `web/src/components/Sidebar.jsx` | workspace 分组渲染 + workspace-scoped session 选择 |
| `web/src/lib/api.js` | workspace-scoped list/create/resume helper |

## 后续 issue

参见 `tasks.md` 第 13 节:
- shared daemon 内的 per-workspace provider/model override
- shared daemon 旧 per-workspace run 目录诊断/清理
- 历史 hash 目录 backfill `workspace.json`
- 折叠状态持久化
- 跨 workspace 全局搜索
- 设置面板 workspace 管理
- POSIX folder picker
