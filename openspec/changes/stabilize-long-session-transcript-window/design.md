## Context

`ChatView` derives `renderedItems` from reducer-owned transcript items, then stores the
first visible row's temporary item id as the tail-window boundary. The boundary is stable
during ordinary token appends, but `transcript_replace` intentionally reconstructs the
visible history for provider retry/recovery. `historyItemsFromMessages` allocates fresh
item ids during that reconstruction even when every persisted message is unchanged.

`windowTranscriptItems` treats a missing boundary as fail-open and returns the complete
projection. That behavior protects content, but `ChatView` only resets its window after an
intermediate empty transcript. A non-empty replacement therefore leaves the stale id in
component state and permanently renders the complete large transcript. The reported case
changed from 122 mounted tail rows to all 270 projected rows after the first of three retry
replacements.

The design must preserve the existing interaction contracts documented in
`docs/web-chat/transcript-window.md`: explicit full expansion, progressive reveal,
conversation find, turn navigation, tail-follow, and manual review.

## Goals / Non-Goals

**Goals:**

- Preserve a transcript window boundary across reconstruction of the same persisted
  messages even when reducer item ids change.
- Recover a bounded tail window synchronously when a replacement genuinely removes the
  old boundary.
- Keep explicit full expansion and progressive reveal semantics stable.
- Exercise the real history-load, projection, replacement, and windowing path in tests.

**Non-Goals:**

- No change to daemon `transcript_replace` generation, provider retry, session storage,
  activity projection, or Markdown rendering.
- No absolute-position or variable-height virtual list rewrite.
- No change to the tail-follow state machine, near-bottom threshold, or automatic scroll
  scheduling.
- No persistent window state across page reloads or session switches.

## Decisions

### Decision 1: Key window boundaries by stable message identity

`transcriptWindowItemKey(item)` will produce a namespaced key. A non-empty persisted
`messageId` is authoritative; otherwise the current item id is an explicitly temporary
fallback. Namespace prefixes prevent a persisted id such as `42` from colliding with
temporary item id `42`.

User rows selected as normal boundaries pass through `transcriptProjection.js` unchanged,
so their `messageId` survives both projection and `transcript_replace` reconstruction.

Alternative considered: preserve reducer item ids across `transcript_replace`. Rejected
because it couples history reconstruction to presentation identity and requires matching
every message/tool expansion shape inside `sessionTranscript.js`.

Alternative considered: reset the window on every replacement. Rejected because retry
replacement normally contains the same messages and should preserve a progressively
revealed historical boundary rather than jump it back to the latest tail.

### Decision 2: Reconcile the boundary before slicing the current render

A pure helper will resolve `(renderedItems, previousAnchorKey)` with three distinct states:

- `undefined`: uninitialized or empty transcript; initialize when rows appear;
- `null`: explicit full view; preserve without re-windowing;
- string key: preserve when present, otherwise select a fresh initial tail boundary.

`ChatView` will compute this result during render, update per-session state only when the
resolved value differs, and use the resolved value immediately for
`windowTranscriptItems`. This prevents even one fail-open full-transcript paint.

Alternative considered: repair the key in `useEffect`. Rejected because the browser would
first mount and lay out the complete transcript, which is the expensive failure being
fixed.

### Decision 3: Keep fail-open as a low-level content-safety fallback

`windowTranscriptItems` will continue returning all rows if called directly with an
invalid key. The main `ChatView` path must reconcile first, while the lower-level helper
retains the rule that windowing bugs cannot hide conversation content.

Alternative considered: make invalid keys return an empty or guessed slice. Rejected
because silently hiding history is a more severe failure than a slow render for an
uncoordinated caller.

### Decision 4: Verify the integration path, not only helper arrays

The focused test will load a large persisted-message array through
`loadTranscriptHistory`, derive the projection, apply a `transcript_replace` through
`reduceTranscriptEvent`, and project again. Assertions will cover stable boundary reuse,
hidden history after replacement, missing-boundary recovery, and explicit full view.

No new test-runner registration is needed because `transcriptWindow.test.js` already runs
in the Web suite.

## Risks / Trade-offs

- [Legacy messages can lack `messageId`] -> Use a namespaced temporary item key and
  synchronously select a new tail boundary if reconstruction invalidates it.
- [A replacement can legitimately delete the anchored message] -> Re-anchor to the normal
  initial tail window instead of permanently fail-opening.
- [Render-phase state adjustment can loop] -> Compare the resolved state with the current
  state and write only on an actual transition.
- [A user explicitly selected full history] -> Reserve `null` for that action and never
  reconcile it into a bounded key.
- [Stable key helpers alter progressive reveal] -> Use the same key function for initial
  selection, lookup, and reveal; retain user-turn alignment and scroll-height compensation.

## Migration Plan

1. Add stable key and reconciliation pure functions with focused tests.
2. Switch the internal `ChatView` window state from `anchorId` semantics to `anchorKey`
   semantics and reconcile before slicing.
3. Run the full Web tests and production build, then validate the OpenSpec change strictly.

Rollback is a direct revert of the frontend helper/component changes. There is no persisted
data migration, protocol version, or configuration change.

## Open Questions

None. The reported event sequence and existing component ownership provide enough evidence
to implement the scoped repair.
