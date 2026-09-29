# Source layout

The C++ tree has six include roots. A quoted project header names its module, for example "utils/paths.hpp" or "agent/agent_loop.hpp". A bare filename is reserved for a header beside the including file. Parent-relative includes and former flat source paths are rejected.

## Groups and dependency direction

| Group | Responsibility | Typical modules |
| --- | --- | --- |
| base | Platform access, primitives, configuration values and shared utilities | utils, platform, config, network, environment, pty |
| domain | Data and policies independent of runtime surfaces | llm, session, permissions, sandbox, memory, skills, hooks |
| adapters | Provider, tool and external service integrations | provider, tool, lsp, computer_use, upgrade |
| engine | Prompt construction and agent execution | prompt, tool_preamble, agent |
| host | Session and background service orchestration | session_host, loop, channels, remote_control |
| apps | CLI, terminal, daemon, web and desktop surfaces | cli, tui, daemon, headless, web, desktop |

Dependencies point toward lower groups. Same-group relationships follow ranks and explicit rules in [src/layers.tsv](../../src/layers.tsv). That table also lists standard-only headers, PA and model-call entry points, tool approval entry points and narrow computer helper contracts. It is the authority when this summary is insufficient. Web ownership/size exemptions remain limited to D21.

## Where a new file belongs

| Responsibility | Location |
| --- | --- |
| OS handle, pipe or terminal primitive | base/platform |
| Generic lock, file transaction or lifetime guard | base/utils |
| Session metadata, history or persistence policy | domain/session |
| Provider protocol and model request execution | adapters/provider |
| Tool implementation or external tool connection | adapters/tool |
| Agent requests, model steps, approval and tool batches | engine/agent |
| Session registry, titles and cross-session operations | host/session_host |
| Terminal input, rendering and application assembly | apps/tui |
| CLI arguments and process dispatch | apps/cli |
| HTTP/WebSocket surface | apps/web |

Business state stays with its owner: model caches in provider, region cache in tool/web_search, desktop workspace state in desktop, command usage in tui. utils/state_file only provides generic file transactions. Do not add business wrappers to generic utilities or forwarding headers at old paths.

## Entry points and testing

[CLI main](../../src/apps/cli/main.cpp) dispatches and runs the selected surface. [TuiApp](../../src/apps/tui/app/tui_app.hpp) owns named initialization stages and normal/exceptional cleanup. [AgentLoop](../../src/engine/agent/agent_loop.hpp) receives fixed services and options, then starts explicitly.

Tests mirror module paths without the group: engine/agent maps to tests/agent, host/session_host to tests/session_host. Shared fixtures use the full test_support prefix. All TUI implementations compile once into the `acecode_tui` static library. Unit tests consume the same production archives through the source-free `acecode_testable` INTERFACE target; executable entry points and the Desktop WebView shell are separate.

## Static library boundaries

[cmake/acecode_layer_libraries.cmake](../../cmake/acecode_layer_libraries.cmake) implements P5. Library names use the `acecode_` prefix:

| Libraries | Ownership and dependencies |
| --- | --- |
| base_core | config, image, ipc, platform, utils and workspace; JSON, SQLite and native OS support |
| base_host | network, pty and environment; depends on base_core and adds CPR/WinPTY or libutil |
| domain | Domain modules; depends on base_core without CPR, Crow or MCP |
| adapters / engine / host | Downward chain; adapters owns external integrations, host adds Crow for channels and remote control |
| web / tui / headless | Application libraries over host; web owns embedded assets, tui owns FTXUI and every TUI implementation |
| daemon / cli | Daemon uses web; CLI assembles daemon, headless and TUI |
| desktop_support | Reusable Desktop code over base_core; no agent, TUI, Crow or Web asset linkage |

Libraries publish their own group include root and receive lower roots from their dependencies. Standalone smoke fixtures retain the six-root include interface. Native computer-use workers remain separate executables and reuse base_core. WinPTY keeps its upstream-specific implementation and embedded agent. Platform-specific and Desktop-off source exclusions are explicit.

Configure-time checks require STATIC layer targets, an INTERFACE test aggregate, downward links and exactly one primary owner per active translation unit. Independent smoke fixtures may compile selected production sources separately. CI independently checks the generated File API graph, complete TUI coverage, Web asset ownership and final consumer link lines. The same check works with `BUILD_TESTING=OFF`. File API queries must exist before CMake starts, including on the supported CMake 3.20 baseline:

```sh
python scripts/refactor/cmake_target_snapshot.py --build-dir build --query
cmake -S . -B build  # retain the toolchain/options used to configure this directory
python scripts/refactor/check_layer_libraries.py --build-dir build
```

## Migration and guards

The [layout mapping](../../scripts/refactor/src_layout_map.tsv) records old and final paths. Keep legacy refs intact, generate a migration patch, resolve semantic extraction conflicts and pass migrate_branch.py --check before merging. No compatibility header is retained at an old path.

Layer, size and ownership checks run in strict mode in CI. CTest registers layer_lint. Ownership targets allow only explicit, bounded primitive/third-party exceptions; the layering exception table stays empty. See [the tooling guide](../../scripts/refactor/README.md) for commands.
