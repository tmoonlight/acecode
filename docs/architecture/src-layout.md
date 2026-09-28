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

Tests mirror module paths without the group: engine/agent maps to tests/agent, host/session_host to tests/session_host. Shared fixtures use the full test_support prefix. The CMake testable TUI allowlist contains implementations testable without the full terminal loop; production links those same implementations.

## Migration and guards

The [layout mapping](../../scripts/refactor/src_layout_map.tsv) records old and final paths. Keep legacy refs intact, generate a migration patch, resolve semantic extraction conflicts and pass migrate_branch.py --check before merging. No compatibility header is retained at an old path.

Layer, size and ownership checks run in strict mode in CI. CTest registers layer_lint. Ownership targets allow only explicit, bounded primitive/third-party exceptions; the layering exception table stays empty. See [the tooling guide](../../scripts/refactor/README.md) for commands.
