## Context

当前基线把 FTXUI 子模块更新到了包含 `EnableMouseHoverMotion` 的提交，但 overlay port 的版本仍指向旧缓存身份，Windows vcpkg 因而可恢复不含该 API 的历史安装并在 `main.cpp` 编译失败。与此同时，链接 host 提取在切分 authority 前搜索最后一个 `@`，技能使用状态只做进程内串行且按本地时区解析 UTC，两个 Python 工具脚本也缺少危险路径和多配置构建边界。

这些问题横跨依赖、TUI runtime、持久化与开发工具，但均属于输入/生命周期边界加固。实现必须保留现有用户数据和公开接口，不修改脏主工作区，不依赖发布侧操作。

## Goals / Non-Goals

**Goals:**

- 让干净 Windows 环境确定性地编译到当前 FTXUI 子模块源码。
- 让 TUI 链接安全判断以浏览器实际 authority 为准，并保证 tooltip 和启动器生命周期有界。
- 让技能使用状态在严格 UTC、损坏字段、多实例和多进程条件下保持可用且不丢更新。
- 让本地验证脚本只构建真实目标、不会删除受保护目录，并能识别常见 Windows/Desktop 构建布局。
- 用聚焦单测和完整 Release 构建证明修复。

**Non-Goals:**

- 不改变链接可点击、OSC 8 或悬停延时等既有产品交互。
- 不迁移技能状态文件格式，不引入数据库或后台服务。
- 不修改 FTXUI 第三方源码，不发布安装包、tag 或远端分支。
- 不重构全部构建预设或解决与本次贡献无关的环境性测试波动。

## Decisions

### 1. 通过 overlay `port-version` 表达 FTXUI 源码身份变化

提升 `ports/ftxui/vcpkg.json` 的 `port-version` 并同步 portfile 注释，使 vcpkg ABI/缓存身份变化并重新安装当前 gitlink。相比在 ACECode 中绕过 API 或修改 vcpkg 缓存，该方式保留当前功能，也符合 overlay port 的版本语义。

### 2. 先切 authority，再处理 userinfo

HTTP(S) host 提取先在 scheme 后找到 `/`、`\\`、`?`、`#` 中最早的 authority 结束位置，仅在该切片内处理最后一个 `@`、端口和方括号 IPv6。这样路径或 query 中的 `@trusted.example` 不能覆盖真实 host。URL 形显示文字或远端 href 无法可靠解析时采用 fail-closed 降级；普通非 URL 标签仍保持可点击。

tooltip 复用现有 cell-width/UTF-8 截断 helper，内容预算由终端宽度推导；极窄终端不画边框浮层。POSIX URL 启动后由分离 waiter 回收直接子进程，避免长期 TUI 产生 zombie。

### 3. 技能状态使用严格解析、类型安全读取和锁文件串行

时间戳只接受 `YYYY-MM-DDTHH:MM:SS[.fff]Z`，先验证公历字段，再用 `_mkgmtime`/`timegm` 转为 UTC epoch；1/2/3 位小数分别按 100/10/1 毫秒缩放。

JSON 字段通过显式类型检查读取；损坏 entry 被视为默认值，写入时正规化为对象，计数饱和而不回绕。每次 mutation 按“实例 mutex → `<state>.lock` 跨进程锁 → 重新读取 → 原子替换”执行。读操作继续依赖原子 rename 读取完整快照，避免无必要阻塞。

### 4. 工具脚本在执行前验证解析后的真实边界

`verify_package.py` 保留用户目标名 `tui`/`desktop`，仅在 CMake 调用处映射为 `acecode`/`acecode-desktop`。Windows 默认让 CMake 选择 Visual Studio generator；非 Windows 才优先 Ninja。staging 路径在任何删除前 resolve，并拒绝文件系统根、用户目录、当前目录、仓库、build 目录及其危险祖先；仓库内仅允许 build 目录的严格后代。

`dev_desktop.py` 保持 Python 3.8 语法，Web 新鲜度覆盖源码、public、manifest/lockfile 和 Vite 配置，Desktop 产物搜索覆盖单配置及常见多配置层级，并安全显示仓库外绝对路径。两个脚本启动的探测子进程均完成 wait/reap。

### 5. 回归测试靠近纯边界

链接和技能状态使用现有 GoogleTest 套件；脚本 helper 使用 Python `unittest`，并在 CTest 找到解释器时注册。四份 verify-package skill 脚本保持字节一致，并以 hash 检查防止副本漂移。

## Risks / Trade-offs

- [Risk] POSIX 文件锁或 Windows `LockFileEx` 在异常退出时留下 `.lock` 文件 → 锁由 OS handle 持有，进程退出即释放；残留空文件不代表锁定。
- [Risk] 严格时间解析会拒绝历史畸形时间戳 → 该 entry 的时间按未知处理，但原文件不删除，后续正常写入会恢复有效值。
- [Risk] staging 边界可能拒绝少见的仓库内自定义目录 → 错误信息明确要求使用 build 子目录或仓库外专用目录，优先保证不可误删。
- [Risk] detached waiter 为每次 POSIX 打开创建短生命周期线程 → 打开链接是低频交互，成本可控，且避免全局 SIGCHLD 行为影响其他子进程。
- [Risk] 提升 port-version 会触发一次 FTXUI 重编 → 这是消除错误二进制缓存所需的预期成本。

## Migration Plan

1. 合入 overlay、runtime、持久化和脚本修复及测试。
2. 在干净 build 目录重新 configure，使 vcpkg 使用新 port-version。
3. 运行 Web、Python、GoogleTest 和 TUI/Desktop Release 构建。
4. 若需回滚，可整体回退本变更；技能状态文件格式未改变，无数据迁移步骤。

## Open Questions

无。
