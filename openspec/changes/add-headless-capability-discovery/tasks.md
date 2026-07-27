## 1. CLI Contract

- [x] 1.1 Add `--list-skills` and `--list-mcp` option state and parsing.
- [x] 1.2 Reject discovery flags combined with prompts or execution-only options.
- [x] 1.3 Update print-mode usage and help with discovery semantics and examples.

## 2. Capability Catalog

- [x] 2.1 Add a deterministic pure formatter for name/description catalogs.
- [x] 2.2 Add the early discovery branch that reuses workspace Skill and enabled MCP data sources without starting agent runtimes.

## 3. Verification and Documentation

- [x] 3.1 Add focused parser, help, catalog-formatting, and conflict tests.
- [x] 3.2 Update English, Chinese, and repository headless documentation.
- [x] 3.3 Build the relevant targets, run focused tests, manually verify both discovery commands, and strictly validate the OpenSpec change.

## 4. Built-in Tool Discovery

- [x] 4.1 Add `--list-tools` parsing, discovery conflict handling, usage, and help text.
- [x] 4.2 Reuse the default headless built-in registration chain and list the exact names accepted by `--disable-tools`.
- [x] 4.3 Extend tests and English/Chinese/repository documentation for all three catalogs.
- [x] 4.4 Build, run focused tests, manually verify tool-only and combined discovery, and strictly validate the expanded OpenSpec.
