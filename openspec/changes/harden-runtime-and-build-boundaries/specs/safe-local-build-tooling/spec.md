## ADDED Requirements

### Requirement: 包验证必须构建真实 CMake 目标
本地包验证器 SHALL 将用户目标 `tui` 和 `desktop` 分别映射为 `acecode` 和 `acecode-desktop`，并在 Windows 非开发者环境中让 CMake 选择可用的默认 generator。

#### Scenario: 同时验证 TUI 与 Desktop
- **WHEN** 用户请求 `--target both`
- **THEN** CMake build 命令包含目标 `acecode` 和 `acecode-desktop`

#### Scenario: Windows 未初始化 MSVC 环境
- **WHEN** Windows PATH 中存在 Ninja 但没有显式 C/C++ compiler
- **THEN** 配置命令不强制选择 Ninja

### Requirement: staging 删除必须受路径边界保护
验证器 MUST 在执行任何递归删除前验证 resolve 后的 staging 路径，并拒绝文件系统根、用户目录、当前目录、仓库、build 目录或这些受保护路径的祖先。仓库内 staging MUST 是 build 目录的严格后代。

#### Scenario: staging 指向仓库根
- **WHEN** 用户将 `--staging-dir` 指向 repo 根目录
- **THEN** 验证器在删除前失败并保留仓库内容

#### Scenario: staging 位于 build 子目录
- **WHEN** staging 是 build 目录的严格后代
- **THEN** 验证器允许清理并重建该 staging 目录

### Requirement: Desktop 探测进程必须完成回收
本地包验证器 SHALL 在结束 Desktop smoke 进程后调用 wait 并确认子进程已退出。

#### Scenario: smoke 超时后终止
- **WHEN** Desktop 在 smoke 窗口内持续运行
- **THEN** 验证器 kill 后等待该进程退出，不遗留未回收子进程

### Requirement: Desktop 开发脚本必须兼容声明的运行环境
开发脚本 MUST 可由 Python 3.8 解析，并 SHALL 对 Web 的源码、public、manifest、lockfile 和 Vite 配置变化触发重建判断。

#### Scenario: lockfile 比 dist 新
- **WHEN** `pnpm-lock.yaml` 的修改时间晚于 `dist/index.html`
- **THEN** Web build 被判定为过期

#### Scenario: Python 3.8 加载脚本
- **WHEN** Python 3.8 解析脚本模块
- **THEN** 不因仅在更高版本支持的类型注解语法失败

### Requirement: Desktop 构建发现必须覆盖常见布局
开发脚本 SHALL 在指定 build 根目录、直接配置子目录和一层 preset/config 子目录中寻找 Desktop 产物，并 SHALL 能显示仓库外的绝对构建路径。

#### Scenario: Visual Studio 多配置产物
- **WHEN** executable 位于 `build/<preset>/Release/acecode-desktop.exe`
- **THEN** 自动发现并选择该产物

#### Scenario: 指定仓库外 build 目录
- **WHEN** `--build-dir` 指向 repo 外且产物存在
- **THEN** 脚本显示绝对路径并继续启动，而不因 `relative_to` 抛出异常
