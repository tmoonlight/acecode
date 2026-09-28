<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md. The option budget lives in `src/tool/ask_user_question_tool.cpp` as anonymous-namespace constants (`kMinOptions=2`, `kMaxOptions=4`) used by validation, the error message, and the tool-definition schema. The config layer already has the exact pattern we need: `AskConfig.max_questions` (default 10, clamped to [1,50] with a warning on load) flows into `create_ask_user_question_tool_async(max_questions)` at three registration sites.

## Goals / Non-Goals

**Goals:** Make the per-question option upper bound configurable (4–8, default 6); keep every limit the model sees (schema, error text) in sync with the configured value; preserve the fixed lower bound of 2; keep behavior identical for existing configs.

**Non-Goals:** Configuring the lower bound; changing over-limit behavior (still a hard error); adding UI to edit the setting (the existing `ask.max_questions` is file-config-only and stays that way); touching TUI/Web rendering.

## Decisions

- Add `AskConfig.max_options` (default 6), parse it from the `ask` object as an integer, clamp to [4,8] with a warning, and mirror it in config dump/validation like `max_questions`.
- Thread the option budget through the tool factories: `validate_ask_user_question_args(args, err, max_questions, max_options)` and `build_ask_user_question_def(max_questions, max_options)`. Effective bounds are clamped once at the boundary of the tool layer so callers cannot bypass the legal range.
- Keep `kMinOptions=2` fixed. Replace the `kMaxOptions=4` constant with a default (`kDefaultAskMaxOptions=6`) plus the legal range constants (`kMinAskMaxOptions=4`, `kMaxAskMaxOptions=8`); the effective max used in validation and schema comes from the config.
- Over-limit requests keep failing with a hard error whose message names the current dynamic upper bound, and the schema `maxItems` reflects the same value, so the model is told the real limit before it can fail.

## Risks / Trade-offs

- Default 6 is a deliberate behavior change from the previous fixed 4: with no configuration, models may now submit up to 6 options. Restoring the old cap requires `ask.max_options: 4`. Recorded in ADR-0001.
- A config file written by a newer build (e.g. `max_options: 8`) read by an older build would be ignored by the older build's schema. Acceptable: the old build still enforces 4, which is a subset of what the newer config allows; this matches how unknown config keys already behave.
- The clamp range [4,8] is enforced in two layers (config load and tool factory). This is intentional defense in depth, mirroring the existing `max_questions` treatment; the two layers agree by construction.

## Migration Plan

No storage migration. New default applies on next startup for configs that omit the key. Users wanting the old cap set `ask.max_options: 4`. Deliver the change as a normal scoped commit on master.
