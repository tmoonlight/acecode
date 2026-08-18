# AGENT.md

This file provides guidance to acecode (https://github.com/tmoonlight/acecode) when working with code in this repository.

## Project Overview

ACECode is a C++17 AI coding agent with a terminal TUI, daemon HTTP/WebSocket API, bundled React web UI, Windows service mode, and optional desktop shell. The shared agent core is reused across surfaces; avoid fixing behavior in only one surface when the same rule belongs in shared code.

Use these durable references when the task touches their area:

- `README.md`: user-facing setup, run modes, prerequisites, and release-facing behavior.
- `ARCHITECTURE.md`: stable runtime map and source ownership.
- `AGENTS.md`: contributor rules, OpenSpec expectations, coding style, and test guidance.
- `CLAUDE.md`: legacy implementation memory and detailed subsystem notes. Keep it on disk unless explicitly asked to remove it.
- `docs/daemon-api.md`: daemon REST/WebSocket protocol. Update it when protocol behavior changes.
- `docs/model-context-resolution.md`: saved model, context-window, and resume rules.
- `docs/desktop-shell/multi-workspace.md`: desktop shell workspace/daemon behavior.
- `docs/subagents.md`: `spawn_subagent` / `wait_subagent` semantics, parent persistence, and TUI/Web bridging.
- `docs/skills.md` and `docs/skills-implementation.md`: skill layout, discovery, and invocation hints.
- `docs/hooks.md`: Codex-compatible lifecycle hooks.

## Common Commands

Fetch submodules:

```bash
git submodule update --init --recursive
```

Build the embedded web UI before configuring CMake when frontend assets should be included in the daemon/desktop binaries:

```bash
cd web
pnpm install
pnpm test
pnpm build
cd ..
```

Configure a testable C++ build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DBUILD_TESTING=ON
```

Common triplets are `x64-linux`, `arm64-linux`, `x64-windows-static`, `x64-osx`, and `arm64-osx`. Windows builds require libcurl 8.14 or newer for TLS behavior.

Build:

```bash
cmake --build build --config Release
```

Build and run all C++ tests:

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```

Run a single discovered GoogleTest via CTest:

```bash
ctest --test-dir build -R <TestNameOrRegex> --output-on-failure
```

Run the unit-test binary directly with a GoogleTest filter when needed:

```bash
./build/tests/acecode_unit_tests --gtest_filter=<SuiteName.TestName>
.\build\tests\acecode_unit_tests.exe --gtest_filter=<SuiteName.TestName>
```

Run local quality checks:

```bash
scripts/code_quality_check.sh
scripts/code_quality_check.bat
```

Common runtime entry points after a build:

```bash
acecode configure
acecode
acecode --resume
acecode --resume <session-id>
acecode -r
acecode --worktree [name]
acecode daemon --foreground
acecode daemon start
acecode daemon status
acecode daemon stop
acecode -p "prompt"
acecode -p --help
```

`acecode -r` is not a `--resume` short alias: it starts the TUI, then opens the session picker. Headless print mode (`-p` / `--print`) is a separate process; `acecode -p --resume <id>` requires an explicit session id because a bare `--resume` would swallow the positional prompt. `-c` / `--continue` resumes the latest ordinary session for the current cwd. `--session-id` must match `[A-Za-z0-9-_]{1,64}`. Discovery flags `--list-tools`, `--list-skills`, and `--list-mcp` exit before stdin, hooks, model, session, LSP, or MCP runtime.

Develop the web UI against a foreground daemon:

```bash
acecode daemon --foreground
cd web
pnpm dev
```

`pnpm test` runs the custom Node runner in `web/src/lib/runTests.js`. There is no Jest/Vitest filter; add or import a `*.test.js` file next to the helper under test. Set `web.static_dir` to a live `web/` tree when the daemon should serve filesystem assets instead of the embedded `web/dist/` snapshot.

Build the optional desktop shell:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DACECODE_BUILD_DESKTOP=ON

cmake --build build --target acecode-desktop
```

On Linux desktop builds, install the WebKitGTK development package before configuring. The desktop target expects the `acecode` daemon executable beside it at runtime.

## Architecture Boundaries

`main.cpp` owns the terminal TUI entry point: CLI parsing for interactive mode, provider/tool/command setup, FTXUI event loop wiring, and worker callbacks back into the UI thread. Keep reusable behavior out of `main.cpp` and place it in the relevant `src/<subsystem>/` area or an existing focused helper.

`src/agent_loop.cpp` is the multi-turn state machine. A text-only assistant reply ends the loop; `task_complete` is an optional explicit terminator. `AskUserQuestion` returns to the loop through an async prompter and is not a terminator. TUI and daemon turns should share this behavior.

`src/tool/` owns the tool registry and built-ins, including shell, file read/write/edit, grep/glob, task completion, skills, memory, MCP, structured user questions, and optional web search. Tool result summaries, metadata, and hunks are consumed by both TUI and web rendering, so keep structured result data centralized instead of formatting separately per surface.

`src/session/` persists canonical conversation messages as `<session-id>.jsonl` plus `<session-id>.meta.json`. Runtime-only display fields are not serialized; resume paths rebuild display rows, tool previews, summaries, and diffs from persisted messages and metadata. File-mutating tools should call the session checkpoint hook before writes so `/rewind` can restore user-turn state.

`src/daemon/` starts and supervises daemon mode, writes runtime pid/port/guid/token/heartbeat files, and owns foreground/detached/service lifecycle. `src/web/` owns Crow routes, WebSocket envelopes, auth, payload codecs, and static asset serving. Daemon session multiplexing goes through `SessionRegistry`; each session entry owns its own `SessionManager`, `PermissionManager`, `AgentLoop`, provider slot, async permission prompter, and question prompter.

`web/src/` is the React 18 + Vite + Tailwind v4 frontend. Prefer pure helpers in `web/src/lib/` for data shaping, protocol handling, markdown rendering, diff handling, and preference persistence. Do not edit generated frontend output directly; rebuild it with `pnpm build`.

`src/desktop/` is an optional native shell around daemon-backed webviews. It manages workspace registry, per-workspace daemon processes, bridge calls, tray menu, notifications, close-to-tray policy, and single-instance behavior. It should not require daemon internals to know about multiple workspaces.

`src/provider/` centralizes `LlmProvider` implementations, provider factory/swap logic, Copilot auth, OpenAI-compatible streaming, saved model profiles, and context-window resolution. TUI `/model` and daemon model switching should both use the shared `apply_model_to_session` path.

`src/network/proxy_resolver.*` centralizes proxy behavior for cpr call sites. Startup initializes the resolver and may probe proxy reachability; `/proxy` is a session-level override and should not persist config changes unless the command explicitly supports that.

`src/skills/`, `src/memory/`, and `src/project_instructions/` provide optional prompt context. Skills are discovered from configured global, project, and external directories and loaded lazily through `skill_view` or slash-command expansion. A compact skill index is injected each request into the never-persisted session-context reminder; `skills_list` is the fallback enumerator. Memory writes are constrained to `~/.acecode/memory/`.

`src/headless/` is the print-mode surface (`acecode -p`). It bootstraps through `SessionRegistry` so sessions land on disk and can be resumed, but it does not write daemon `run/` files or start Crow. `headless::active()` auto-denies tool permissions (unless `--yolo`) and auto-answers `AskUserQuestion`. New `-p` sessions that omit `--enable-skills` / `--enable-mcp` register neither; `--disable-tools` removes named builtins. Allowlists are runtime-only and do not persist.

`src/worktree/` isolates git worktrees under the main-repo `.acecode/worktrees/<slug>` on branch `worktree-<flatten(slug)>`. `EnterWorktree` / `ExitWorktree` are registered on both TUI and daemon; call them only when the user explicitly asks for a worktree. Switching cwd goes through `AgentLoop::set_cwd` and rebuilds `PathValidator`; session storage stays on the original project. Mid-session worktrees are throwaway working copies. Merging a worktree branch into master/main is not an exit — the session stays in the worktree until `ExitWorktree` runs, which is what clears the Desktop worktree badge. CLI `--worktree` / `-w` is different: the worktree becomes the process project root (logs, workspace registration, and session storage all live inside it). `ExitWorktree remove` is fail-closed unless `discard_changes` is set.

`spawn_subagent` / `wait_subagent` create a normal `SessionRegistry` child with the parent's cwd and permission mode. Children persist `parent_session_id`, are hidden from ordinary session lists (`GET /api/sessions?parent=<id>` lists them), cannot spawn further children, and surface only in the parent's background-task UI. The TUI main session is not in the registry; its permission mode reaches children through `SubagentToolDeps::fallback_permissions`. See `docs/subagents.md`.

`src/lsp/` is a process-local LSP client pool. Queries and post-edit diagnostics must use the session cwd from `ToolContext`, not the daemon process cwd, and must `weakly_canonical` paths before workspace-boundary or slot-key compares. Servers are detected from PATH / the project; ACECode never downloads language servers.

Permission modes live in `src/permissions.hpp`: `Default` (prompt writes/exec), `AcceptEdits` (auto-allow file writes, still prompt shell), `Yolo` (auto-allow tools), and `Plan` (explore / write only the active plan file). Memory writes stay path-locked even under broader modes. Non-loopback `--dangerous` is rejected.

Other shared subsystems that are easy to miss: `src/gitinfo/` (prompt + REST git snapshots), `src/hooks/` (Codex-compatible lifecycle hooks), `src/web/pty/` (console dock; `/api/pty` and `/ws/pty` are loopback-only), `src/loop/` (scheduled loop tasks), `src/experts/`, and `src/remote_control/`.

## Test And Build Structure

`acecode_testable` is the shared object library for headless logic used by production binaries and tests. Most of `src/tui/` and `src/markdown/` stay out of it; a few pure TUI helpers are compiled into `acecode_testable` so unit tests can cover them. `acecode` links `acecode_testable`, `acecode_native_bridge_support`, and the remaining TUI/markdown sources. `acecode-desktop` must not link `acecode_testable`; desktop-only sources stay in `acecode_desktop_support` / the desktop binary. The `acecode_unit_tests` target is discovered from `tests/**/*_test.cpp` and uses GoogleTest. Keep TUI-heavy and desktop-webview-only behavior isolated unless logic can be moved into a pure helper covered through `acecode_testable`.

Tests live under `tests/` and usually mirror source paths. Use focused tests for serializers, parsers, validators, handler helpers, provider/model helpers, permission logic, and headless state machines. Web-only changes should at minimum run `pnpm test` and `pnpm build` from `web/`.

## Protocol And Persistence Notes

Loopback daemon requests bypass token auth. Non-loopback requests require `X-ACECode-Token` or `?token=`, and non-loopback dangerous mode is rejected. WebSocket events use envelopes with `type`, `seq`, `timestamp_ms`, and `payload`; `EventDispatcher` assigns monotonic sequence numbers and keeps a bounded replay ring.

The daemon and TUI share session storage under the ACECode data directory. Current sessions use canonical shared JSONL files; old `<session-id>-<pid>.jsonl` files are unsupported experimental data and should not be migrated.

Saved model selection resolves through named `saved_models`, `default_model_name`, per-project overrides, and resumed session metadata. If no saved model is configured, normal startup fails instead of synthesizing a fallback. If protocol or metadata behavior changes, update the relevant docs and tests alongside the code.

`WebServer::Impl::app_config_mu` is a `shared_mutex`. Routes that only read or snapshot config, including the two resume paths that parse JSONL, must take `std::shared_lock`. Settings writes, `saved_models` persistence, and `refresh_default_session_preferences` take `std::lock_guard<std::shared_mutex>`. Concurrent resume of the same id is serialized by `SessionRegistry::resume_inflight_`.

Attention persistence is throttled. `note_session_event_for_attention` runs on the AgentLoop worker that emits the event; the hot path only dirties a workspace set, and a 1s flusher writes the file. Do not write the attention file on every Token / Reasoning / Tool event.

## Load-Bearing Invariants

Prompt-cache prefix: `build_api_request_messages()` runs every sampling iteration. Content injected before the last real user message must be byte-stable when inputs are unchanged. Time-varying text in that prefix busts the cache for the rest of the turn (user message plus every later assistant/tool message). Cwd and calendar date belong in the static system-prompt `# Environment` block. Same reason `cached_context_for_api` is pinned by cache key, git snapshots are session-cached, `ToolExecutor::tools_` is a `std::map`, and Anthropic synthetic `tool_call_id`s are content hashes. Guarded by `agent_loop_termination_test.cpp::RequestPrefixIsByteStableAcrossIterationsInATurn` and the byte-stable cases in `system_prompt_test.cpp`.

Cross-provider usage: `prompt_tokens` is the full input of that request; `cache_read_tokens` is a subset. Anthropic reports input and cache counters separately; `AnthropicProvider::merge_usage` folds them into that contract. If a provider reports usage but no cache counters, surfaces show 0% rather than hiding the chip.

Writer leases under the project session directory serialize TUI and daemon activation of the same transcript. Stale leases recover when the owner PID is dead or the heartbeat is old. File-mutating tools still call `SessionManager::track_file_write_before` so `/rewind` can restore the user-turn checkpoint.

## Repository Rules To Preserve

For non-trivial behavior changes, create or continue an OpenSpec change under `openspec/changes/` before implementation and update its `tasks.md` as tasks are completed.

The Superpowers plugin is disabled for this repository. Do not invoke or follow `superpowers:*` skills unless the user explicitly re-enables Superpowers for a specific turn.

Keep TUI-specific code in `main.cpp`, `src/tui/`, and `src/markdown/`. Keep daemon/API work in `src/daemon/`, `src/web/`, and shared session/provider/tool helpers as appropriate. Keep frontend work under `web/src/`. Avoid modifying vendored or submodule trees such as `external/`, `hermes-agent/`, or `claudecodehaha/` unless the task explicitly targets them.

Follow `.editorconfig`: UTF-8, LF endings, final newline, 4-space indentation for C++/CMake, and 2-space indentation for JSON/YAML. Use C++17. Headers should generally live in `src/**/*.hpp` with matching `.cpp` files. Test files use the singular `_test.cpp` suffix.

Prefer existing helpers such as `ToolArgsParser`, `ToolErrors`, path/session utilities, provider/model helpers, web handler pure functions, and frontend `web/src/lib/` helpers instead of duplicating parsing, validation, or protocol logic.

This is a terminal UI project. Avoid emoji and ambiguous-width glyphs in C++ source, rendered UI, logs, and console output; prefer ASCII or width-stable symbols already used in the codebase.

On Windows PowerShell 5.1, do not use `Set-Content` or `Out-File` for Chinese or other non-ASCII text; they can write a BOM or mojibake. Use editor-based edits or explicit UTF-8-without-BOM APIs.

Treat `--dangerous` / `--yolo` as a local-only sandbox convenience. Do not recommend it without a clear warning. Do not commit API keys, Copilot tokens, generated session data, local config, daemon tokens, or memory files.
