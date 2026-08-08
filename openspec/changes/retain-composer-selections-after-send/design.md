## Context

`ChatView` owns the current composer attachments, contexts, swarm-mode flag, and expert selection. Submission currently calls one shared cleanup callback: it correctly removes one-shot attachments and contexts, but also resets swarm mode. Expert selection follows a different lifecycle: home selection is copied into the new session, while existing sessions persist and restore `expert_id` through the session reference.

The fix must keep those existing backend contracts intact and distinguish a submission cleanup from a real conversation-context change.

## Goals / Non-Goals

**Goals:**

- Keep swarm mode selected after every successful chat submission, including first-message promotion and queued input.
- Keep the selected expert visible and bound after submission by retaining the existing `expert_id` persistence and restoration path.
- Continue clearing one-shot composer data after successful submission.
- Retain an explicit reset when the composer changes to a different home/session context.

**Non-Goals:**

- Changing swarm prompt semantics, daemon request validation, or the message metadata schema.
- Persisting swarm mode as new backend session metadata across application restarts.
- Changing expert switching, queuing, or package management behavior.

## Decisions

1. Separate one-shot submission cleanup from conversation-context reset.
   - The shared submission cleanup will remove attachments, pinned contexts, and transient selection state only.
   - A distinct context-reset callback will additionally disable swarm mode when `draftSessionKey` identifies a genuinely different composer context.
   - This is preferred over passing `preserveSwarm` flags at every send site because persistence becomes the default and future send paths cannot accidentally clear the selection.

2. Preserve first-message promotion explicitly.
   - Home-to-session promotion changes `draftSessionKey` as part of the same send. The existing promotion guard will continue to suppress the context reset when swarm mode is selected, so the new session composer retains the selection.

3. Reuse expert session persistence.
   - No new expert state store is needed. Home creation already includes `expert_id`, session references restore it, and only the explicit remove/switch actions mutate it.
   - Regression coverage will guard submission cleanup from calling either swarm or expert reset setters.

## Risks / Trade-offs

- [A future send path calls the context-reset callback instead of the one-shot cleanup] -> Keep the callbacks separately named and add an architecture regression that inspects the cleanup boundary.
- [Swarm mode leaks into a different conversation] -> Keep the `draftSessionKey` effect responsible for the explicit context reset, except for the intentional home-to-session promotion guard.
- [Queued messages differ from the current selection after a later user change] -> Preserve the existing queue snapshot behavior; each queued payload retains the selection active when that message was queued.
