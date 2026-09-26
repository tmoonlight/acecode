# Repository Guidelines

## Project Structure & Module Organization

ACECode is a C++17 terminal AI coding agent with daemon, web UI, and optional desktop surfaces. The root [main.cpp](main.cpp) owns the terminal TUI entry point. Reusable logic lives under [src/](src) by subsystem, including `commands`, `config`, `daemon`, `desktop`, `history`, `markdown`, `memory`, `network`, `project_instructions`, `provider`, `session`, `skills`, `tool`, `tui`, `utils`, and `web`.

Unit tests live in [tests/](tests) and mirror source paths; for example, session storage code should be covered under `tests/session/`. Static model catalog assets are in [assets/models_dev/](assets/models_dev). User and subsystem docs live in [docs/](docs). Vendored or submodule code is under [external/](external). vcpkg overlay ports are under [ports/](ports).

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

- Keep TUI-specific code in [main.cpp](main.cpp), [src/tui/](src/tui), and [src/markdown/](src/markdown). Put reusable, testable logic in the relevant `src/<subsystem>/` area or a focused top-level `src/*.cpp` helper when that is the existing local pattern.
- `acecode_testable` intentionally excludes the full TUI entry point and desktop WebView shell. Pure helpers can be added there and covered by unit tests.
- Daemon/API work usually touches [src/daemon/](src/daemon), [src/web/](src/web), and [src/session/](src/session). Update [docs/daemon-api.md](docs/daemon-api.md) when protocol behavior changes.
- React/Vite/Tailwind frontend work stays under [web/src/](web/src). Do not edit generated build output directly; regenerate it with the web build.
- Avoid modifying vendored or submodule trees such as [external/](external), `hermes-agent/`, or `claudecodehaha/` unless the task explicitly targets them.

## OpenSpec Workflow

For non-trivial behavior changes, create or continue an OpenSpec change under `openspec/changes/` before implementation. Use the repository's OpenSpec workflow commands or the local `openspec` CLI to propose, apply, and archive changes. During implementation, read the change context, complete tasks one by one, and update task checkboxes in the change's `tasks.md` immediately after each task is complete.

## Project-Level Agent Overrides

The Superpowers plugin is disabled for this repository. Do not invoke or follow `superpowers:*` skills, including `superpowers:using-superpowers`, unless the user explicitly re-enables Superpowers for a specific turn.

## Coding Style & Naming Conventions

Follow [.editorconfig](.editorconfig): UTF-8, LF line endings, final newline, 4-space indentation for C++ and CMake, and 2-space indentation for JSON/YAML. Use C++17. Keep headers in `src/**/*.hpp` and implementations in matching `.cpp` files where practical. Name test files with the singular suffix `_test.cpp`; `tests/CMakeLists.txt` discovers that pattern automatically.

Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, path/session utilities, provider/model helpers, and web handler pure functions instead of duplicating parsing or validation logic.

This is a terminal UI project. Avoid emoji or ambiguous-width glyphs in C++ source, rendered UI, logs, and console output. Prefer ASCII or width-stable symbols already used in the codebase.

## 所有权与生命周期

以下 C1–C14 约定来自 [refactor20260927 所有权设计](openspec/changes/refactor20260927-adopt-ownership-conventions/design.md)。新代码遵守这些约定;存量问题按该系列任务逐项整改,不借此扩大一期范围。下面的反例指向重构前的实现,搬迁时同步更新路径。

1. **C1 独占所有权**:拥有对象用 `std::unique_ptr`,包括 pimpl。裸 `new` 只允许用于私有构造工厂中的 `std::unique_ptr<T>(new T)`,禁止裸 `delete`。只有确实共享寿命才用 `shared_ptr`,声明处说明共享方与原因。反例:[web_search/runtime.hpp](src/tool/web_search/runtime.hpp) 的 `Impl* impl_` 在 [runtime.cpp](src/tool/web_search/runtime.cpp) 中手工 new/delete。
2. **C2 构造注入**:必填依赖用构造注入的引用成员;可选指针注明 nullable、borrowed。构造完成后不再更换依赖。反例:[SessionRegistryDeps](src/session/session_registry.hpp) 的 `tools` 可空,但 [make_entry_locked](src/session/session_registry.cpp) 会解引用它。存量依赖结构改为引用注入属于二期。
3. **C3 借用不跨调用**:形参与 `ToolContext` 中的 `T*`/`T&` 不得保存为成员或被异步回调、线程捕获;跨调用使用持有寿命的快照。反例:[spawn_subagent_tool.cpp](src/tool/spawn_subagent_tool.cpp) 把 `child->skill_registry.get()` 存入 `child_skills`,在持有 child 的局部 `shared_ptr` 离开作用域后继续使用。
4. **C4 共享只读快照**:共享只读数据使用 `shared_ptr<const T>`,只由一个发布点更新;不把共享可变对象的子对象地址交给其它线程。反例:[session_registry.cpp](src/session/session_registry.cpp) 的 `set_project_instructions_config(deps_.project_instructions_cfg)` 保存借用配置,而 [settings_mutations.cpp](src/config/settings_mutations.cpp) 可以整份替换配置。
5. **C5 异步捕获**:listener、control、AgentCallbacks、工具闭包和线程入口只能捕获值、自有状态的 `shared_ptr`、`weak_ptr` 或 `LifetimeRef`,禁止裸指针、`[this]`、`[&]`。唯一例外是回调由宿主持有的 `JoiningThread` 执行,且在所依赖成员析构前 join。反例:[session_registry.cpp](src/session/session_registry.cpp) 的 `on_turn_finished = [this, id]` 与 [worker.cpp](src/daemon/worker.cpp) 的 on_spawn 对 server 的引用捕获。
6. **C6 不形成自持环**:对象自己的队列或成员回调不得强持有该对象;捕获 `weak_ptr`,执行时 lock 并核对身份。反例:[session_registry.cpp](src/session/session_registry.cpp) 的 `enqueue_control([entry, loop, ...])` 形成 entry → loop → 队列 → lambda → entry。
7. **C7 订阅即资源**:使用 move-only 的 `ScopedSubscription`,声明在被捕获对象之后;析构时退订并等待在途投递。持有 listener 需要的锁时不得析构订阅。反例:[headless_runner.cpp](src/headless/headless_runner.cpp) 在 `subscribe` 后的 send_input 失败分支早退,绕过末尾 `unsubscribe`。
8. **C8 线程有宿主**:禁止新增裸 `std::thread` 成员、局部变量与直接 detach。长期线程用 `JoiningThread`,短任务用 `ReapingThreadSet`;可放弃等待的阻塞工作只经 `run_abandonable` / `spawn_owned_detached`。原语实现自身的 detach 例外须封装并记录。反例:[routes_workspaces.cpp](src/web/routes/routes_workspaces.cpp) 的导入线程 detach,以及 [session_registry.hpp](src/session/session_registry.hpp) 只增不减的 `vector<std::thread>`。
9. **C9 进程级服务**:由组合根的 RAII Scope 管理 init/shutdown;全局访问只返回 `shared_ptr` 租约,不采用 `is_initialized()` 后再 `service()` 的两步访问。反例:[lsp_tool.cpp](src/tool/lsp_tool.cpp) 的两步式 LSP 获取。存量服务租约化在二期,一期新代码先遵守。
10. **C10 关停顺序**:停入口 → 停回合生产者 → `SessionRegistry::shutdown_all()` → MCP → LSP/web_search → 其余,由成员声明顺序或显式关停序列固化。反例:[worker.cpp](src/daemon/worker.cpp) 在 registry 析构前关闭 MCP/LSP;D6 按所有权 change 单独提交。
11. **C11 析构契约**:持有 worker 的成员必须声明在其依赖成员之后;做不到时写显式析构函数先停 worker。反例:[SessionEntry](src/session/session_registry.hpp) 的 ask_prompter 比 loop 先析构;[SubagentHost](src/tui/subagent_host.hpp) 的 registry 比其回调访问的 mu_、running_ 后析构。
12. **C12 不延迟绑定依赖**:依赖 registry 的工具应在 registry 构造后注册,闭包捕获 `weak_ptr<Service>`。反例:[headless_runner.cpp](src/headless/headless_runner.cpp) 先注册工具、再回填 `subagent_deps->registry` 与 `thread_deps->service`。一期 O-06 只修退出后的悬垂,注册顺序重构留二期。
13. **C13 句柄 RAII**:OS 与第三方句柄使用 move-only 封装,不通过 `void*` 出参交付所有权,不在多个出口手工 Close。反例:[sandbox_backend.hpp](src/sandbox/sandbox_backend.hpp) 的 `void*` 句柄接口与 [computer_use/runtime.cpp](src/computer_use/runtime.cpp) 可复制的 Handle。
14. **C14 锁序**:LifetimeToken、AbortSignal、GoalRuntime 的内部状态锁是叶子锁,持锁时不进入 registry、AgentLoop 或业务回调;持有 `model_control_mu` 时不得获取队列门。现有风险入口是 [session_registry.cpp](src/session/session_registry.cpp) 的 `switch_model` 在 model 锁内调用 `apply_model_to_session`;迁移时不能在这条调用链内新增队列加锁。该文件的 `set_reasoning_effort` 已明确 queue → model → binding → metadata 的正确顺序,不得反转。

所有权棘轮 R15 的度量与一期目标以所有权设计为准。新增原语必须有覆盖取消、关停、回调在途与析构顺序的测试;既有豁免与二期待办不得通过新增同类问题扩大。

## Testing Guidelines

Tests use GoogleTest through the `acecode_unit_tests` target. Add tests for pure logic, serializers, parsers, validators, handler helpers, and headless state machines. Keep TUI-heavy code in [src/tui/](src/tui), [src/markdown/](src/markdown), and [main.cpp](main.cpp) manually validated unless logic can be isolated.

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

## Security & Configuration Tips

Do not commit API keys, Copilot tokens, generated session data, local config contents, runtime daemon tokens, or memory files with private user data. Treat `--dangerous` mode as a local-only sandbox convenience and avoid recommending it without a clear warning.

On Windows PowerShell 5.1, avoid `Set-Content` and `Out-File` for files containing Chinese or other non-ASCII text because they can write BOMs or mojibake. Use editor-based edits or explicit UTF-8 without BOM APIs instead.
