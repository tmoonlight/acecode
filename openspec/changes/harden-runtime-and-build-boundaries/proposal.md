## Why

对近期贡献代码做全量回归后，发现当前远端基线存在一处可稳定复现的 Windows 编译阻断，以及链接边界、技能使用状态和本地构建脚本中的安全与可靠性缺口。这些问题会分别导致错误的依赖缓存被复用、可见 URL 与实际打开主机不一致、使用记录丢失或时间计算错误，以及验证脚本误删目录或构建错误目标，因此需要在继续发布前统一修复。

## What Changes

- 同步 FTXUI 子模块与 vcpkg overlay 的版本标识，确保包含悬停 API 的真实源码被重新构建，恢复 TUI/Desktop 的干净编译。
- 收紧 TUI 超链接 authority/host 解析，处理路径、查询或反斜杠中的 `@`，对畸形 URL 形链接默认降级，并修正窄终端 tooltip 尺寸及 POSIX 启动器子进程回收。
- 将技能使用时间戳改为严格 UTC 解析，容忍损坏或旧 schema 字段，并通过跨进程锁保护 read-modify-write，避免并发写入丢失。
- 修复本地包验证脚本的 CMake 目标映射、Windows generator 选择、staging 删除边界和 Desktop 子进程回收；修复 Desktop 开发脚本的 Python 3.8 兼容性、Web 输入新鲜度判断、构建产物发现和仓库外路径显示。
- 为上述边界补充回归测试，并执行 Web、脚本、单元测试以及 TUI/Desktop Release 构建验证。

## Capabilities

### New Capabilities

- `tui-hyperlink-boundary-safety`: 约束链接 host 解析、畸形链接降级、窄终端悬停显示和平台启动器生命周期。
- `durable-skill-usage-state`: 约束技能使用状态的 UTC 时间语义、损坏数据兼容和跨进程原子更新。
- `safe-local-build-tooling`: 约束本地验证与 Desktop 开发脚本的目标选择、删除边界、输入新鲜度和产物发现。

### Modified Capabilities

无。

## Impact

- C++ runtime：`src/markdown/link_safety.cpp`、`src/utils/open_url.cpp`、`src/main.cpp`、`src/skills/skill_usage_store.*`。
- 依赖打包：`ports/ftxui/*`，不修改第三方 FTXUI 源码。
- 本地工具：四份同步的 `verify_package.py` 和 `scripts/dev_desktop.py`。
- 测试：`tests/markdown/`、`tests/skills/`、`tests/scripts/` 及测试注册配置。
- 不改变 daemon/Web API，不引入用户配置迁移，也不发布包或标签。
