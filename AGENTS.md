# Repository Guidelines

## src 分层搬迁交付记录

2026-09-29 用户最新决定:取消等待 06:00 候选窗口,一期现已完成约定的 Windows 验收,立即提交并 push。原公告中的最早开始时间由本条覆盖;通过受保护 master 所需的 PR 完成交付,九个旧 ref/worktree 与已批准的补验安排保持。

2026-09-28 公告:按 [重构设计 D3 / §8.3](openspec/changes/refactor20260927-restructure-src-layers/design.md),P3-02 候选冻结窗口为 **2026-09-29 06:00–18:00(Asia/Taipei,UTC+08:00)**。最早开始时间保留至少一天的公告期;只有 P2 全部验收、P3-01 演练通过后才进入窗口,未满足时顺延并更新本公告。

2026-09-29 已完成主线搬迁并解除冻结:[PR #87](https://github.com/tmoonlight/acecode/pull/87) 已合入 master,post-src-layout 指向 1483069f2b97cde11953da1e5e15c8601e71f464。pre-src-layout、机械提交 blame 登记和 Windows 验收记录均已完成。九个旧 ref/worktree 保留原状,迁移另行安排;不得将保留误记为弃用。

2026-09-28 D27(用户最新指令,优先于 D26 和旧的逐任务流程):剩余一期全部在当前 master 检出实施,先完成全部实现,最后集中做 Windows 本机全量验收,完成后一起提交并 push;中途不新建任务分支/worktree、不逐任务提交/推送、不跑跨端 CI。过程中只做必要的编辑/迁移一致性核对;实施状态与验收状态分别记录。跨端补验暂留后续安排,一期实现范围与 D6–D9 行为约定保持。详情见 restructure-src-layers/design.md D27。

2026-09-29 用户补充决定:P3-03 的九个旧分支保留原状,迁移另行安排;不推进或删除原 ref/worktree,不标记弃用,不阻塞本次主线一期交付。

2026-09-29 验收范围补充(用户确认):人工专项后补,本次按 Windows 自动化及已完成实测交付;未执行的人工项目继续记录,不宣称通过,不阻塞本次主线交付。

2026-09-29 交付收尾最新指令(用户确认):发完后确保 CI/CD 已启动即关机,结果明天查看。正常触发 test,在最终 master 上启动 refactor-matrix(全量及 Deepin)和 package 分支构建验证;此前暂停跨端 CI 的安排由本条覆盖。人工专项与旧分支迁移仍按已批准范围后补。

2026-09-28 D26:一期剩余任务的验收只做 Windows 本机(design.md §7.4 轻量协议:静态闸门 + 复用目录的 Ninja 增量构建 + 用例清单 / target 快照对照,内容改动再跑 `python scripts/refactor/run_fast_tests.py --profile fast`),不 dispatch refactor-matrix、不等 test.yml;macOS / Linux / Deepin 与 package.yml 推迟到 tasks.md 5.4「多平台补验」一次做完。

## Project Structure & Module Organization

ACECode is a C++17 coding agent with terminal, daemon, web and optional desktop surfaces. [CLI main](src/apps/cli/main.cpp) dispatches into [TuiApp](src/apps/tui/app/tui_app.hpp). Source modules live in six groups: base, domain, adapters, engine, host and apps. Follow [the source layout guide](docs/architecture/src-layout.md); [src/layers.tsv](src/layers.tsv) is the checked dependency policy.

映射版本: refactor20260927/P3, src_layout_map.tsv SHA-256 17990beaa768f4d7de19c9a6df5d3fabfe0db1ba915eaee64fef5dd02bf5c459。原 ref/worktree 保留,旧路径不留转发头;按映射生成 patch、处理语义冲突并通过 migrate_branch.py --check 后才允许合入。

Unit tests live in [tests/](tests) and mirror module paths without the group prefix; for example, session storage code should be covered under `tests/session/`. Static model catalog assets are in [assets/models_dev/](assets/models_dev). User and subsystem docs live in [docs/](docs). Vendored or submodule code is under [external/](external). vcpkg overlay ports are under [ports/](ports).

Canonical root docs are [README.md](README.md), [README_CN.md](README_CN.md), [ARCHITECTURE.md](ARCHITECTURE.md), this file, and [CLAUDE.md](CLAUDE.md). Keep the root small: do not reintroduce one-off status reports, root task lists, captured build logs, or duplicate project-instruction docs.

## Build, Test, And Development Commands

- `git submodule update --init --recursive`: fetch required submodules.
- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=<triplet> -DVCPKG_OVERLAY_PORTS=$PWD/ports -DBUILD_TESTING=ON`: configure a testable build.
- `cmake --build build --config Release`: build the `acecode` executable.
- `cmake --build build --target acecode_unit_tests`: build the GoogleTest binary.
- `ctest --test-dir build --output-on-failure`: run the unit suite.
- `scripts/code_quality_check.sh` or `scripts/code_quality_check.bat`: run local quality checks for common DRY and error-handling issues.
- In [web/](web), run `pnpm install` once, `pnpm test` for JS tests, and `pnpm build` before configuring/building CMake when embedded frontend assets must be refreshed. See [web/README.md](web/README.md).
- Desktop shell is opt-in: configure with `-DACECODE_BUILD_DESKTOP=ON` and build `acecode-desktop`. The desktop target expects the daemon executable beside it at runtime.

## Architecture Boundaries

- Keep terminal UI behavior in [src/apps/tui/](src/apps/tui), including its markdown and command adapters. Put reusable logic in the owning module under base, domain, adapters, engine or host; keep the CLI entry point limited to dispatch and assembly.
- `acecode_testable` is an INTERFACE aggregate of the production static libraries. All TUI implementations belong to `acecode_tui`; do not add testable subset lists. Executable entry points and the desktop WebView shell stay outside the aggregate. See `cmake/acecode_layer_libraries.cmake` for source ownership.
- Daemon/API work usually touches [src/apps/daemon/](src/apps/daemon), [src/apps/web/](src/apps/web), and [src/domain/session/](src/domain/session). Update [docs/daemon-api.md](docs/daemon-api.md) when protocol behavior changes.
- React/Vite/Tailwind frontend work stays under [web/src/](web/src). Do not edit generated build output directly; regenerate it with the web build.
- Avoid modifying vendored or submodule trees such as [external/](external), `hermes-agent/`, or `claudecodehaha/` unless the task explicitly targets them.

## OpenSpec Workflow

For non-trivial behavior changes, create or continue an OpenSpec change under `openspec/changes/` before implementation. Use the repository's OpenSpec workflow commands or the local `openspec` CLI to propose, apply, and archive changes. During implementation, read the change context, complete tasks one by one, and update task checkboxes in the change's `tasks.md` immediately after each task is complete.

## Project-Level Agent Overrides

The Superpowers plugin is disabled for this repository. Do not invoke or follow `superpowers:*` skills, including `superpowers:using-superpowers`, unless the user explicitly re-enables Superpowers for a specific turn.

## Coding Style & Naming Conventions

Follow [.editorconfig](.editorconfig): UTF-8, LF line endings, final newline, 4-space indentation for C++ and CMake, and 2-space indentation for JSON/YAML. Use C++17. Keep headers in `src/**/*.hpp` and implementations in matching `.cpp` files where practical. Name test files with the singular suffix `_test.cpp`; `tests/CMakeLists.txt` discovers that pattern automatically.

Project headers are included in module-root form relative to the six group include roots (`#include "utils/paths.hpp"`, `#include "tui/tui_state.hpp"`); parent-relative paths (`../`) are rejected by the `layer-lint` job (refactor20260927 P1). A bare name is only for a header in the same directory. Shared test helpers live under `tests/test_support/<area>/` and are always included with that full prefix (`#include "test_support/agent/stub_provider.hpp"`). Before rebasing an older branch, run `python scripts/refactor/normalize_includes.py --scope src` and `--scope tests` on it; the tool rewrites only the quoted path.

Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, path/session utilities, provider/model helpers, and web handler pure functions instead of duplicating parsing or validation logic.

This is a terminal UI project. Avoid emoji or ambiguous-width glyphs in C++ source, rendered UI, logs, and console output. Prefer ASCII or width-stable symbols already used in the codebase.

## 所有权与生命周期

以下 C1–C14 约定来自 [所有权设计](openspec/changes/refactor20260927-adopt-ownership-conventions/design.md)。新代码遵守这些约定;二期事项保持登记,不得新增同类问题。

1. **C1 独占所有权**:拥有对象和 pimpl 用 std::unique_ptr。裸 new 仅限有登记的私有构造工厂,禁止裸 delete。shared_ptr 声明处解释共享方。
2. **C2 构造注入**:必填服务用构造引用,可空借用指针注明 nullable/borrowed。AgentLoopServices 在构造时固定,两个自有 prompter 只能在 start 前安装。SessionRegistryDeps 的全面引用注入留二期。
3. **C3 借用不跨调用**:ToolContext 和参数中的指针/引用不能存入异步闭包。跨调用使用自有快照、weak_ptr 或 LifetimeRef;进入后再同步借用。
4. **C4 共享只读快照**:AgentLoop 每回合捕获 SessionPromptConfig、技能和专家快照。daemon 在配置共享锁下复制,TUI 发布不可变配置;技能和专家变更经 control 生效。
5. **C5 异步捕获**:listener、control、AgentCallbacks、工具闭包和线程入口只捕获值、自有状态 shared_ptr、weak_ptr 或 LifetimeRef。JoiningThread 的宿主捕获例外必须保证依赖析构前 join。
6. **C6 不形成自持环**:对象自己的队列/回调不得强持有对象。SessionRegistry 控制项只保留弱 entry,执行前验证 registry 身份与关停状态。
7. **C7 订阅即资源**:ScopedSubscription 为 move-only,析构退订并等待在途投递。不得持有 listener 需要的业务锁退订;自身/嵌套投递按 EventDispatcher 的调用栈规则处理。
8. **C8 线程有宿主**:长期任务用 JoiningThread,短任务用 ReapingThreadSet。可放弃阻塞工作只能用 run_abandonable/spawn_owned_detached,捕获自有状态,原语级 detach 例外必须登记。
9. **C9 进程级服务**:新服务由组合根 RAII Scope 管理,访问返回租约,避免先判断初始化再借用全局实例。存量 MCP/LSP/web_search 全面租约化留二期。
10. **C10 关停顺序**:停入口和回合生产者,再停所有会话,然后 MCP、LSP/web_search 等依赖。三入口结束时最多等待自有可放弃工作 2 秒;迟到工作只访问保留的租约。
11. **C11 析构契约**:worker 声明在依赖之后,或显式析构先停 worker。SessionEntry/SubagentHost 显式关停;AgentLoop join 后移出双队列,唤醒取消回执,锁外释放闭包后不再访问 this。
12. **C12 不延迟绑定依赖**:新工具在 registry 构造后注册,闭包捕获 weak_ptr<Service>。headless 存量回填在清理时先停 registry 再清指针;完整注册顺序重构留二期。
13. **C13 句柄 RAII**:OS/第三方句柄使用 move-only UniqueHandle/UniqueFd/UniqueProcess/UniqueSqlite 等包装;sandbox 返回自有包装,原生 API 的裸参数只是调用内借用。写者租约由 WriterLease 管理。
14. **C14 锁序**:LifetimeToken、AbortSignal、GoalRuntime 的状态锁为叶子锁,锁内不进入业务回调。model_control_mu 内不获取队列门;现有 queue → model → binding → metadata 顺序不得反转。

所有权棘轮 R15 的度量与一期目标以所有权设计为准。新增原语必须有覆盖取消、关停、回调在途与析构顺序的测试;既有豁免与二期待办不得通过新增同类问题扩大。

## Testing Guidelines

Tests use GoogleTest through the `acecode_unit_tests` target. Add tests for pure logic, serializers, parsers, validators, handler helpers, and headless state machines. Keep TUI-heavy code in [src/apps/tui/](src/apps/tui), [src/apps/tui/markdown/](src/apps/tui/markdown), and [CLI main](src/apps/cli/main.cpp) manually validated unless logic can be isolated.

Use `testing::TempDir()` or `std::filesystem::temp_directory_path()` for file I/O; do not write test artifacts into the repository tree. Prefer `EXPECT_*` unless failure would make later assertions unsafe.

If CMake test discovery/build integration is unavailable in the editor, still keep changes compatible with the documented `cmake --build` and `ctest` commands. For web-only changes, at minimum run `pnpm test` and `pnpm build` from [web/](web).

## 研发实施中的通用注意事项

- 重构或迁移功能时，先明确新旧路径的责任边界，再逐步切换调用入口；如果新旧状态、事件处理或兼容分支同时生效，同一个输入可能被重复处理，问题通常只在特定交互顺序下暴露。
- 事件驱动程序需要明确每类事件的唯一所有者。键盘、鼠标、定时器、重绘和后台回调应经过统一适配层进入业务状态机，不能只迁移最常见的事件而遗漏边缘输入路径。
- 业务状态、传输格式和展示文本应分层维护。不要用格式化后的字符串推断状态；空字符串、缺失字段和显式的空值可能代表不同语义，跨线程或跨进程传输时应保留必要的结构化信息。
- 不要把布局、分页或超时等动态行为写成固定常量。可视区域、终端尺寸、配置值和运行时状态变化后，固定步长或固定边界容易产生越界、跳过内容或无法操作的问题。
- 将时间、外部 IO、线程调度和平台资源封装在边界上，核心逻辑尽量使用可注入的时钟、输入和依赖。这样既能避免测试永久等待，也能稳定覆盖超时、取消和竞态场景。
- Windows 增量构建前确认没有残留进程占用输出文件，并加载正确的编译器开发环境；链接错误有时来自文件锁或环境变量缺失，而不是源代码错误。
- 多步骤任务应采用“小范围修改 → 定向编译/测试 → 再扩大范围”的节奏。遇到失败先判断是代码错误、环境问题、并发进程、缓存还是测试基线问题，不要在未定位原因前反复重试。
- 修改前后都要检查工作区范围。不要使用会清理或覆盖无关用户文件的命令；提交时精确选择相关文件，并通过 `git diff --check`、差异审查和测试结果确认改动没有夹带无关内容。
- 非平凡行为变更应同步更新设计文档、任务清单和测试；不要等全部代码完成后才补记录，否则容易遗漏已验证的约束和未完成事项。

## Commit & Pull Request Guidelines

Recent history uses short imperative commits, sometimes with `feat:` prefixes, for example `feat: Implement AskUserQuestion tool` or `Add unit tests for session serialization`. Keep commits focused and mention tests when relevant.

Pull requests should describe the behavior change, list verification commands, link related issues or OpenSpec changes, and include screenshots or terminal captures for visible TUI, web, or desktop changes.

Mechanical refactor commits (pure include rewrites and pure moves) are listed in [.git-blame-ignore-revs](.git-blame-ignore-revs); run `git config blame.ignoreRevsFile .git-blame-ignore-revs` once so `git blame` skips them.

## Security & Configuration Tips

Do not commit API keys, Copilot tokens, generated session data, local config contents, runtime daemon tokens, or memory files with private user data. Treat `--dangerous` mode as a local-only sandbox convenience and avoid recommending it without a clear warning.

On Windows PowerShell 5.1, avoid `Set-Content` and `Out-File` for files containing Chinese or other non-ASCII text because they can write BOMs or mojibake. Use editor-based edits or explicit UTF-8 without BOM APIs instead.
