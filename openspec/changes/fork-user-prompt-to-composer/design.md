<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

`POST /api/sessions/<id>/fork` takes `at_message_id`, finds its index, and keeps
`retained_prefix_before_index(messages, idx + 1)` — the prefix **including** the
clicked message. `SessionManager::fork_session_to_new_id` then copies that prefix
into a fresh JSONL, filtering file checkpoints, turn timing, and turn net diff
records, and re-attaching timing/diff rows per retained user uuid.

Messages are a flat JSONL array (`ChatMessage` in
`src/provider/llm_provider.hpp:14`); there is no parent/child tree, so "the
previous agent summary" has to be located by scanning backwards.

The Web client's `forkAndSwitch` (`web/src/components/ChatView.jsx:3390`) only
toasts and switches; it never touches the composer. Note that switching sessions
reloads the composer draft in an effect at `:1680`, which clears the value unless
`preserveComposerInputOnSessionChangeRef` is set first.

## Goals / Non-Goals

**Goals:**

- Forking on a user message produces a session that ends at the state **before**
  that prompt ran.
- The clicked prompt's text is refilled into the composer so it can be edited and
  resubmitted.
- Same behavior on desktop (context menu) and Web (hover action) since both share
  `forkAndSwitch`.
- Never leave a dangling assistant `tool_calls` message without its results.

**Non-Goals:**

- Auto-submitting the refilled prompt. (Goose does this; Codex and Grok Build do
  not. Waiting for the user is the safer default and matches the request.)
- Restoring attachments or `content_parts`. Only the plain text is refilled.
- Changing fork behavior for assistant messages.
- Changing the on-disk session schema or the compact/checkpoint model.

## Decisions

1. **Anchor = first real message at or before `idx - 1`.**

   Scan backwards from the clicked user message and skip records that are not
   part of the conversation: `is_meta`, file checkpoints, turn timing, and turn
   net diff (the same predicates `fork_session_to_new_id` already filters). Stop
   at the first remaining message and keep `[0..j]`.

   If nothing real exists before the clicked message (it is the first message),
   keep an empty prefix.

2. **Stop at a `tool` result; do not skip it looking for an assistant.**

   This is the subtle part. An agent turn is
   `assistant(tool_calls)` → `tool(result)` → `assistant(summary)`. Because the
   result comes *after* the call, truncating at the result keeps a complete pair,
   while skipping the result and stopping on the calling assistant would leave a
   `tool_calls` message with no response, which providers reject. Grok Build
   relies on the same invariant by always cutting at user boundaries.

   As a defensive extra, if the anchor lands on an assistant message that still
   has unsatisfied `tool_calls`, keep scanning backwards. Goose omits this guard.

3. **Consecutive user messages keep everything except the clicked one.**

   If the scan reaches another user message first, stop there. A user who sends
   two prompts in a row and forks on the second one expects the first to survive;
   silently dropping it would lose work.

4. **The server is the single source of truth for the restored prompt.**

   The fork response gains `restored_prompt` (plain text of the clicked user
   message) and `fork_anchor_role`. The client refills whenever
   `restored_prompt` is present, so front-end and back-end cannot disagree about
   whether the message was a user prompt, and the client does not need the
   message list to be fully loaded.

5. **Apply a destination-scoped pending draft after switching.**

   `forkAndSwitch` records the returned prompt with the destination draft key.
   It leaves the source composer unchanged so its effect cleanup can save the
   actual source draft. The destination draft effect consumes the matching
   payload, bypasses loading the initially empty server draft, and enables normal
   autosave. A payload for another session is discarded.

6. **Only plain text is refilled.**

   `content_parts` may carry images or files. Restoring them would require the
   composer to accept prefilled attachments, which is out of scope.

## Risks / Trade-offs

- **A fork can now produce an empty session.** Forking on the very first user
  message keeps no history. This is intended (it is how you restart a session
  with a reworded first prompt), but the empty state must render cleanly.
- **Attachments on the original prompt are silently dropped.** Accepted for now;
  the text is refilled and the user can re-attach. Revisit if it causes
  confusion.
- **Resubmitting unchanged is allowed.** It simply replays the turn in the new
  session and cannot affect the source session.
- **Anchor resolution depends on the meta predicates staying in sync** with
  `fork_session_to_new_id`. Reuse the existing helpers rather than duplicating
  the conditions so both evolve together.

## PR integration repairs

PR 46 is integrated alongside PR 47 at the user's request. Build commands use
CMake `--parallel` so Visual Studio and Ninja receive appropriate native flags.
Desktop discovery defaults to the complete build root; explicit directories
still restrict discovery. macOS portable packaging uses separate architecture
defaults, rejects incompatible existing caches, selects matching vcpkg triplets,
always builds incrementally, checks all packaged executables with `lipo`, and
passes a normalized full output path to `ditto`. Script tests mock macOS tools
on other hosts; they do not replace native macOS compilation or launch checks.
