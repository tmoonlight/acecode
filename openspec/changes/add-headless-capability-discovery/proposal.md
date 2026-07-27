## Why

Headless print mode already accepts exact built-in tool, Skill, and MCP names
through `--disable-tools`, `--enable-skills`, and `--enable-mcp`, but users have
no CLI path to discover which names are valid in the current workspace and
configuration. This turns useful capability selectors into guesswork and makes
the examples in `-p --help` incomplete.

## What Changes

- Add `acecode -p --list-tools` to print the currently registered built-in tool
  names accepted by `--disable-tools` in a default headless invocation.
- Add `acecode -p --list-skills` to print the installed, platform-compatible,
  globally enabled Skills available in the current workspace.
- Add `acecode -p --list-mcp` to print configured MCP server names that are not
  globally disabled.
- Allow all discovery flags in one invocation without a prompt, model startup,
  session creation, hooks, tool execution, or MCP connections.
- Reject combining discovery mode with a prompt or execution-only options so
  stdout stays an unambiguous catalog rather than a mixed catalog/agent stream.
- Update `acecode -p --help` and the English/Chinese headless documentation to
  show how to discover names before passing them to `--disable-tools`,
  `--enable-skills`, or `--enable-mcp`.

## Capabilities

### New Capabilities

- `headless-capability-discovery`: Defines prompt-free CLI discovery of the
  exact built-in tool, Skill, and MCP names accepted by headless capability
  selectors.

### Modified Capabilities

None.

## Impact

- Headless CLI parsing and help text under `src/headless/`.
- Headless startup sequencing so discovery exits before agent/runtime startup.
- Focused headless option and catalog-formatting tests.
- `README.md`, `README_CN.md`, and the repository headless behavior notes.
