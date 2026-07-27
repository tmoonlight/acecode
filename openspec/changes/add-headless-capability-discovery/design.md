## Context

`acecode -p` validates `--disable-tools` against the built-in definitions
registered in its `ToolExecutor`, validates `--enable-skills` against a
workspace-aware `SkillRegistry`, and validates `--enable-mcp` against the
non-disabled entries in `AppConfig::mcp_servers`. Those catalogs already define
the selector contract, but they are currently only exposed after a user
supplies an invalid name.

Discovery is a terminal CLI action rather than an agent turn. It must not read
stdin, create or resume a session, dispatch startup hooks, initialize a model,
start MCP transports, or mix its catalog with normal headless output.

## Goals / Non-Goals

**Goals:**

- Expose the exact current built-in tool, Skill, and MCP selector names through stable,
  prompt-free `-p` flags.
- Reuse the same tool registration, configuration, and Skill discovery policy
  as execution-time validation.
- Produce deterministic, readable output that remains useful when either
  catalog is empty.
- Make discovery visible at the point where users learn about
  `--enable-skills` and `--enable-mcp`.

**Non-Goals:**

- Executing a built-in tool or connecting to MCP servers to enumerate their
  tools or health.
- Enabling globally disabled Skills or MCP servers.
- Adding JSON output, category filtering, fuzzy matching, or a separate
  top-level subcommand.
- Persisting any allowlist or changing normal `-p` execution defaults.

## Decisions

### Add three combinable terminal flags

`--list-tools`, `--list-skills`, and `--list-mcp` directly match the three
selectors and can be combined to print any requested catalogs. A single generic
capability command was considered, but separate flags are easier to discover
beside their matching selector options and avoid inventing another value
grammar.

Discovery mode accepts no prompt and no execution-only option. Rejecting mixed
invocations is preferable to silently ignoring flags or contaminating an agent
result stream with catalog text. `--help` retains its existing highest
precedence.

### Reuse selector data sources without executing capabilities

Built-in tool discovery constructs the same default headless tool registry used
before `--disable-tools` validation and reads only definitions whose source is
`ToolSource::Builtin`. Registration may construct inert tool objects and local
web-search routing state, but it does not invoke any tool, start LSP, dispatch
hooks, or create a session. Skill discovery initializes an unrestricted
registry from the loaded config and current working directory; this naturally
filters globally disabled and platform-incompatible Skills and applies the same
root precedence as a normal headless run. MCP discovery reads configured
entries whose `disabled` field is false. It does not construct
`DaemonMcpRuntime`, so listing cannot launch an MCP process or make a network
connection.

The discovery branch runs before prompt assembly because prompt assembly can
wait for or consume redirected stdin.

### Keep output deterministic and human-oriented

Each requested catalog has a heading with its count. Entries are sorted by
their exact selector name; Skills include a whitespace-normalized description
when present, while built-in tools and MCP entries show names only. Empty
catalogs print `(none)`. When multiple flags are supplied, built-in tools are
printed first, followed by Skills and MCP servers.

The first version remains text-only. Script callers can still extract exact
names one-per-entry, while a future JSON format can be added without changing
selector semantics.

### Share a pure catalog formatter

A small headless helper formats generic name/description entries. Keeping this
logic independent of configuration, model, session, and transport code allows
deterministic unit coverage and avoids duplicating output rules in the runner.

## Risks / Trade-offs

- [A Skill can change on disk between discovery and execution] → Both commands
  rescan from disk, so each invocation reports and validates its own current
  snapshot; no stale cache is introduced.
- [A configured MCP can be listed even if its executable or endpoint is broken]
  → The heading and help describe configured, enabled servers rather than
  connected/healthy servers; discovery intentionally performs no side effects.
- [Human-readable descriptions complicate shell parsing] → Exact names remain
  on dedicated indented entry lines and MCP output is name-only; structured
  output is explicitly deferred.
