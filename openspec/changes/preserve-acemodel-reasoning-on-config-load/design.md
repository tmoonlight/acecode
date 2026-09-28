<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The built-in ACEModel catalog supplies stable vision and tool tags. Upstream `/models` probing can additionally save explicit reasoning options for the same profile. `parse_one_entry` currently overwrites all capability tags from the built-in catalog, while `validate_saved_models` correctly requires a saved reasoning declaration and capability tag to agree. Both the active config and last-good candidate use this parse path.

## Goals / Non-Goals

**Goals:** Keep catalog defaults and an explicit saved upstream reasoning declaration coherent in memory during every config load.

**Non-Goals:** Change reasoning discovery, API payloads, user-selected manual capabilities, or general validation rules.

## Decisions

- In the existing ACEModel catalog normalization branch, derive the effective tags from the built-in catalog and append `reasoning` only when the parsed, valid saved reasoning declaration says it is supported. The declaration is the only evidence for a dynamic reasoning tag; model names alone do not imply it.
- Keep the repair in the shared saved-model parser. It covers TUI, daemon, Desktop, active config, and last-good validation without adding a special recovery bypass.
- Leave the source JSON unchanged. A catalog-sourced profile is interpreted against the current built-in catalog on each load; any later normal config save writes the coherent effective tags. This also avoids rewriting a credentials-bearing config merely for derived metadata.

## Risks / Trade-offs

- [An upstream model later stops declaring reasoning] → A fresh probe can clear the saved declaration through the existing model editor flow; startup cannot infer a remote change without probing.
- [Malformed saved options] → Parsing and validation remain strict, so genuine invalid data still takes the existing recovery path.
