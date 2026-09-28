<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

# Design

## Context

See `proposal.md` for motivation and `specs/development-environment-launcher/spec.md` for behavioral requirements. The repository already has Python-backed Web and Desktop launchers with thin platform wrappers. Web currently accepts an explicit build directory and runtime directory, while Desktop accepts an explicit build directory and rebuilds `web/dist` when its inputs are newer. TUI has no comparable wrapper.

## Goals / Non-Goals

**Goals:**

- Provide one shared Python orchestration entry point and thin Windows/POSIX wrappers.
- Reuse existing Web and Desktop launch scripts rather than duplicating their surface-specific behavior.
- Reliably identify compatible builds from Git-registered worktrees before proposing a local build.
- Keep development daemons isolated per worktree and make start outcomes visible.

**Non-Goals:**

- Change production daemon, Desktop, TUI, or CMake behavior.
- Copy build artifacts, DLLs, or resources between worktrees.
- Search arbitrary user directories outside worktrees registered by the current Git repository.
- Automatically compile without interactive confirmation.

## Decisions

### Use a Python orchestration layer with thin platform wrappers

A new `scripts/dev_environment.py` will parse the selected target and coordinate discovery, confirmation, incremental building, and launch. Dedicated target wrappers will select Python and a fixed target: `dev_web.bat` / `dev_web.sh`, `dev_desktop.bat` / `dev_desktop.sh`, and `dev_tui.bat` / `dev_tui.sh`. Python matches the existing launcher implementation and is portable across Windows, macOS, and Linux.

Alternatives considered:

- Separate shell implementations would duplicate platform logic and Git/CMake parsing.
- Extending `dev_web.py` or `dev_desktop.py` would couple target-independent discovery to a single surface.

### Discover builds only from Git worktree registrations

The orchestration layer will inspect only the current worktree's candidate build directories and read `CMakeCache.txt` to confirm that each build was configured from the current source directory. It validates platform, generator architecture clues, selected target configuration, and executable presence. Registered peer worktrees are used only for safe compiler-cache and frontend-artifact acceleration; their CMake/Ninja directories are never built or launched for the current worktree.

Alternatives considered:

- Scan sibling or home directories: this can find unrelated repositories and is slower.
- Compare only executable timestamps: timestamps do not prove the source revision or Desktop capability.

### Delegate surface-specific startup

The shared launcher performs the target's incremental CMake build after it validates or configures a build directory. For Web and Desktop, it then invokes the existing Python surface launchers directly, forwarding the validated `--build-dir`; this avoids recursing through target wrappers. The Desktop surface launcher continues to refresh frontend assets, and the shared launcher adds the equivalent freshness check before Web launch. Web receives a deterministic runtime directory below the current worktree's ignored development state. TUI is started from the validated `acecode` binary in a new terminal window using platform-specific process invocation.

Alternatives considered:

- Reimplement Web and Desktop startup in the new tool: would create two sources of truth for Web assets and Desktop development-mode behavior.
- Run TUI in the current terminal: conflicts with the agreed ability to continue launcher work after opening TUI.

### Auto-configure missing builds from Windows direct entry points

Double-clicked batch files do not provide a reliable input stream for the shared launcher's confirmation prompt. Each Windows target entry point will therefore append `--yes` when it calls `dev_environment.py`, approving only the missing-build configuration path. The shared Python launcher remains conservative for callers that invoke it directly, and POSIX wrappers retain their interactive confirmation behavior.

### Initialize the Windows C++ toolchain in batch entry points

Windows 包装脚本先选择 Python，公共启动器识别候选构建后才决定是否需要 MSVC。MinGW 构建直接使用已有编译器；MSVC 路径通过共享 `dev_windows_env.bat` 查询匹配 x64 或 ARM64 的 VS 组件并初始化环境。Python 只在子进程内捕获环境变量，再传递给后续编译，不输出环境内容。帮助、列表和 dry-run 不触发工具链初始化。Windows ARM64 对应的两个默认 configure presets 与 x64 使用相同的基础配置。

Alternatives considered:

- Require callers to use a Developer Command Prompt: contradicts direct double-click and normal-shell entry point behavior.
- Duplicate Visual Studio path discovery in all three wrappers: risks inconsistent architecture and error handling.

### Require interactive confirmation only for a new configuration

When discovery cannot produce a compatible result, the tool calculates the native CMake preset for the selected target and prints configure/build commands. It asks a yes/no question only when stdin is interactive; non-interactive invocations fail with the same instructions rather than implicitly configuring. Once a build directory has been validated or configured, the target's incremental build runs without another confirmation so each launch reflects current sources. First-time development configurations pass `-DBUILD_TESTING=OFF`, because unit-test dependencies are optional in the vcpkg manifest and are not needed to run a development surface.

Alternatives considered:

- Auto-build: potentially expensive and unexpected.
- Always fail: forces developers to reconstruct platform-specific presets manually.

## Risks / Trade-offs

### 发布审查收敛

- 多配置构建记录实际产物的配置名称，并通过 `cmake --build --config` 编译同一配置；仅修改编译缓存 launcher 时保留已有 `CMAKE_BUILD_TYPE` 和 `BUILD_TESTING`。独立的嵌套 preset 不属于父级 CMake 构建。
- 公共启动器向 Desktop 传递已验证的具体产物，向 Web 传递该可执行文件所在目录，避免二次发现选到其他配置。`--list` 只列举产物；`--rebuild` 强制刷新前端。
- Windows TUI 直接创建新控制台进程；macOS 用 Terminal 的 AppleScript 入口执行经过逐参数 shell 引用的 `cd` 和 `exec`，确保工作目录与参数一致。
- Web 重建前遇到既存 PID 记录时，复用原有 `daemon status` 身份校验并明确失败。此次不引入跨平台进程管理框架，也不调用现有仅按 PID 终止的 `daemon stop`。仅在核验成功后显示停止命令供开发者操作；无法确认身份时只显示检查命令。默认目录检查包含当前工作树旧提交的 runtime，显式目录只检查自身。
- 回归使用临时目录与替身命令；Windows 另验证真实 VS 环境初始化。macOS/Linux 图形终端和 ARM64 原生编译不在本轮 Windows 主机验证范围内。

- [A valid external build is configured with an unusual directory layout] → inspect standard build directories plus explicit `--build-dir`; require an exact `CMakeCache.txt` source match.
- [CMake does not expose a fully portable architecture field] → require a runnable platform-native executable and compare configured generator/platform fields when present; reject uncertain configurations.
- [A worktree-specific runtime path is untracked] → create it under the repository's ignored `.acecode` development state and document it in the launcher output.
- [A platform lacks a supported graphical terminal launcher] → report the exact TUI executable command instead of silently starting TUI in the caller's terminal.

## Migration Plan

1. Add the shared launcher and thin wrappers without changing existing Web or Desktop launch commands.
2. Add the repository-local skill, directing requests through the shared policy.
3. Validate no-target selection, verified reuse, rejected reuse, declined rebuild, and each platform wrapper through focused tests or script help checks.
4. Roll back by removing the new launcher, wrappers, and skill; existing Web and Desktop launchers remain unchanged.
