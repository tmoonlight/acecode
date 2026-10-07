## Context

The user approved all five decisions on 2026-10-07. `ApiRequestBuilder::build` currently repositions context on every call. Hidden session records and compact checkpoints already provide append-only persistence. Ordinary/star spawning leaves an unspecified model empty, while mesh already inherits the parent. OpenAI-compatible transports currently omit cache keys.

## Goals / Non-Goals

Goals: preserve identical model-facing prefixes across unchanged turns and reloads, keep context updates effective without rewriting prior input, and preserve all summary/tool safety checks. See proposal.md for the five approved changes.

Non-goals: changing user-visible transcript layout, claiming a measured cache hit percentage, changing provider/model pricing, adding model-specific GLM/Qwen features outside the five decisions, or publishing a release.

## Decisions

1. Persist versioned hidden request-context records in session history. A snapshot supplies a frozen skill index and initial session context at the beginning of the current window; subsequent session/hook/plan/security updates are append-only hidden user context. Internal records remain hidden to clients and are not real user turns. Recognized records survive effective-history reconstruction; unrelated meta records remain excluded. This replaces the moving-context rule in `replicate-current-codex-compaction/design.md` section 10. An in-memory-only pin was rejected because restart would change the prefix.
2. A successful compaction installs a new snapshot together with the replacement history/checkpoint, before retained user messages and the final compact summary. Current todos are included once in that new window (and when bootstrapping a legacy history that has no snapshot); normal TodoWrite results remain the source of incremental checklist state. Failed compaction does not reset the window.
3. The request builder exposes one compaction request assembly path using the same frozen snapshot, model-facing history transformation, system prompt, and tool definitions as normal requests. Summarization appends only its instruction. OpenAI-compatible providers reuse tools; `tool_choice: none` is emitted only where supported. Native/textual tool calls invalidate a summary and trigger the existing no-tool fallback. Other providers retain their safe legacy path.
4. Ordinary/star subagents without a model selection inherit the parent's current saved model. Explicit selection wins; absent parent state retains the existing configured fallback. Mesh behavior stays equivalent and gains regression coverage.
5. Cache keys use the real session ID (a stable per-loop fallback only without a session), carried by value in each request rather than set on shared providers. Only verified HTTPS official OpenAI/Mistral endpoints enable `prompt_cache_key` by default. Unknown/custom endpoints receive no extra field. A clear unsupported-field 400/422 removes only that field for one retry and remembers the decision for that endpoint/model in the provider instance. Authentication, quota, cancellation and unrelated failures keep existing semantics. Provider capability state is synchronized for concurrent readers.

## Risks / Trade-offs

- Older instructions are farther from the newest message -> explicit updates describe replacement scope; permission enforcement remains authoritative in tool execution.
- Hidden records could leak into UI or user-turn predicates -> reuse meta hiding, recognize only versioned internal records, and test persistence/reload and compaction.
- Reusing tool schemas can provoke a tool response -> disable choice where supported and validate both native and textual tool diagnostics before accepting a summary.
- Cache keys cannot ensure server residency -> verify serialized requests and usage-compatible behavior locally; do not invent numerical savings.

## Migration Plan

Existing histories receive one initial snapshot on the next normal request. New histories/checkpoints persist their snapshot; reload uses its bytes. No bulk rewrite of stored sessions is required. Implementation remains on the current checkout, with narrow edits preserving unrelated Web work. Validate targeted regressions, then the Windows unit suite and static/OpenSpec checks.

## Verified implementation boundaries

- Explicit memory enable/disable starts a new context epoch. Disabling memory removes earlier automatically injected memory from outgoing context, taking priority over cache reuse. Old session records remain on disk; conversation messages and hook events are preserved. The latest valid snapshot supersedes earlier snapshot/state-update records in the model projection.
- Skill usage/dormancy changes do not alter the frozen system index. A real catalog or tool-policy change appends a replacement index alongside other context updates; it does not silently hide newly configured expert skills until compaction.
- Only validated versioned records participate in context reconstruction. Corrupt fields and transcript-only snapshots cannot replace a valid epoch or cause typed JSON reads to throw.
- A context record must be durably appended before it enters live history or a normal provider request. A compact checkpoint must be durable before publishing replacement history, window counters or success notices. Fresh-window preparation uses separate cache state so failed persistence does not refresh the live window.
- Workspace/model/tool-schema changes can still change their earlier prompt sections. The byte-stability guarantee covers unchanged configuration and append-only context updates, not every possible model or workspace switch.
