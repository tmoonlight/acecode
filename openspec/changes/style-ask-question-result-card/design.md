<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

AskUserQuestion already has two UI surfaces: the active picker/overlay while the agent waits for input, and the completed tool result after the user answers. This change only targets the completed result.

The current model-visible output is a single-line text string produced by `format_ask_answers()`. That string is part of the tool-result contract and feeds back to the provider. Desktop/web also receives structured tool lifecycle events, but successful `tool_end` events normally omit output and rely on summary or generic tool folding. TUI renders `tool_result` text directly when a tool has no `ToolSummary`.

## Goals / Non-Goals

**Goals:**

- Keep the completed AskUserQuestion result as tool transcript content rather than merging it into the assistant bubble.
- Render desktop/web completed results with a compact card matching the provided visual direction.
- Preserve complete answer text for the model and persisted history.
- Make the result resumable from persisted session metadata.
- Keep TUI behavior readable with the existing tool-result text path and a better tool-call preview.

**Non-Goals:**

- No picker/overlay redesign while the question is pending.
- No WebSocket protocol change for `question_request` or `question_answer`.
- No change to AskUserQuestion validation, answer semantics, or failure output.
- No mandatory TUI graphical card matching every desktop detail such as hover tooltips.

## Decisions

### Store structured Q/A as ToolResult metadata

Add a generic `ToolResult::metadata` JSON object and have `ToolExecutor::format_tool_result()` copy it to the persisted `ChatMessage.metadata`. `AgentLoop` also emits the same metadata in `tool_end` payloads for live desktop/web rendering.

Rationale: this keeps the model-visible output stable while giving UI code a structured, durable source of truth. Parsing the existing answer string would be fragile because questions and answers may contain quotes, commas, or translated text.

Alternative considered: encode the UI data in `ToolSummary.metrics`. Rejected because metrics are flat label/value pairs and would not represent ordered Q/A rows cleanly.

### Let ToolBlock render the card

Desktop/web should treat the completed AskUserQuestion answer as a specialized tool block. `ToolBlock` can switch to a `QuestionResultCard` when the tool entry contains `askUserQuestionResult` metadata.

Rationale: the user accepted keeping it as tool display. This avoids reshaping assistant messages and preserves existing activity grouping rules for other tools.

### Preserve full output for copy and model use

The visible card clamps individual Q/A text to two lines. The underlying output and metadata keep full text. Native tooltips are added only for actually clamped text by measuring rendered height after layout.

Rationale: the UI remains compact without losing data for provider context, copy actions, resume, or future rendering.

### Keep TUI on the text path

Do not attach `ToolSummary` to AskUserQuestion. The TUI already preserves tool-result line breaks and can show the improved text result. The shared preview path should show a readable AskUserQuestion call summary.

Rationale: adding summary would collapse the result into a single line and hide the Q/A unless expanded.

## Risks / Trade-offs

- [Risk] Generic `ToolResult::metadata` may be overused by future tools. Mitigation: document it as UI/persistence metadata and keep AskUserQuestion keys namespaced.
- [Risk] Line-clamp tooltip detection depends on DOM measurement. Mitigation: isolate it in a small component and fall back to no tooltip when measurement is unavailable.
- [Risk] Persisted sessions need the card after resume. Mitigation: normalize `metadata.ask_user_question_result` from persisted tool messages into tool entries.

## PR #49 persistence repair

- Use the existing shared ToolBlock renderer for both submission and cancellation. The PR's proposed ChatView `renderAfterItem` extension and feedback modules were absent from its committed tree; a second per-view card renderer would duplicate the existing result surface.
- Explicit cancellation carries `ask_user_question_result.cancelled=true` and an empty `items` array. Preserve `success=false` and the existing provider-visible rejection text. Legacy rejection records without this metadata continue to use their generic tool output.
- Normalize only namespaced question-result metadata. Generic tool metadata such as `cancelled=true` must not create an AskUserQuestion card.
- Treat structured question metadata as the source of truth for card visibility, including renamed tools and history slices missing the assistant call. Cancellation takes precedence over stale answer rows and remains outside processed summaries.
- Resolve persisted tool names in message order, clearing pending calls at user-turn boundaries and replacing stale names when a later assistant call reuses an ID. Consume each result once; preserve explicit tool names and never inspect future calls.
- Exercise the actual ToolBlock JSX with React server rendering in the persistence tests. A synthetic sequence of labels alone cannot prove that the application renders the card.
