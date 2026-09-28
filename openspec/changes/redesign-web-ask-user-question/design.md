<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The interaction and visual decisions are recorded in `docs/specs/2026-09-13-web-ask-user-question-requirements.md` and `docs/design/web-ask-user-question-design.md`. Their 2026-09-14 revisions define numeric-key advancement, custom-input Enter, and two-stage Escape. The delta spec includes those revisions.

The daemon accepts the first answer for an entire request. Transcript self-heal replaces item identifiers, and ChatView can remain mounted across sessions.

## Goals / Non-Goals

Preserve local drafts until final batch submission and bind durable feedback to its own result. Keep the current protocol, picker appearance, composer replacement, and existing daemon interjection support.

## Decisions

- Separate selection toggling from confirmation. Confirmation ensures that its option remains selected before navigation; it cannot undo the first click of a double-click.
- Resolve keyboard targets from explicit hover/focus, then recommendation, then the first option. IME composition is handled before question shortcuts.
- Derive feedback from completed tool metadata inside shared ToolBlock rendering. Remove the unbound ChatView transient fallback: selecting the latest tool by name can attach a previous result to a new request or another session. The brief wait for `tool_end` keeps feedback authoritative.
- Persist question type and unanswered status using additive metadata fields. Existing history without type metadata remains readable.

## Risks / Trade-offs

- Browser mouse and IME event ordering can differ from pure state tests. Validate the real component event handlers and run browser interaction checks.
- Feedback appears when the tool result arrives. Until then, the tool remains pending and does not claim that a different request completed.

## Migration Plan

No stored-history migration is required. Validate the repaired Web implementation and existing native metadata tests before integration.
