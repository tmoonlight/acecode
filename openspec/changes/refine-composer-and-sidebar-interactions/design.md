# Design

## Context

See proposal.md for motivation. The integrated drafts use existing UI preferences,
recommendation gating, queued-message dispatch, the filesystem API and custom
pointer dragging. This document records the implementation being prepared for
release; it does not rewrite prior delivery chronology.

## Goals / Non-Goals

**Goals:** Keep one persisted preference source, preserve legacy defaults, and
reuse existing request sequencing and drag cleanup paths.

**Non-Goals:** Change model generation prompts, server queue semantics, filesystem
permissions, touch/pen behavior, or the operating system's drag-lock settings.

## Decisions

- Store `follow_up_prompts` and `busy_send_mode` in `web_ui`; use the existing
  serialized appearance persistence controller for updates. Browser-only storage
  would not reliably survive desktop daemon port changes. Backend gating prevents
  an older client from spending model requests after predictions are disabled.
- Resolve busy-send preference through a pure helper shared by the composer and
  chat dispatch. Existing queues, abort handling and fork dispatch retain ownership
  of their state transitions. Duplicating action rules in click and key handlers
  would risk mismatched hints and behavior.
- Render the folder creation modal as a sibling of its picker so portal event
  bubbling does not route its keys through the picker's navigation handlers. Use
  the existing mkdir endpoint and preserve validation errors inside the dialog.
- Check mouse `buttons` before crossing the drag threshold, then invoke existing
  cancellation logic. Increasing a distance threshold would only conceal a stale
  press; time-based guesses would interfere with ordinary long presses.

## Risks / Trade-offs

- Mac drag locking can legitimately report the primary button held after a tap.
  This guard cannot distinguish that from a real drag. The user requested an
  attempted fix; Mac hardware verification remains outstanding.
- Desktop bootstrap does not inject the new fields. The existing preferences GET
  restores them, and server-side prediction gating remains authoritative meanwhile.
- Existing main `busy-input-queue` specs describe an older two-button UI. Current
  production steer/queue dispatch and its in-flight changes already supersede that
  presentation. This change specifies the preference layer and does not pretend
  to reconcile every older unarchived behavior document.
- Concurrent development tests do not establish the final release snapshot.
  Wait for task/background completion, then review source, run integrated checks,
  and record actual results before committing and tagging.

## Migration Plan

Absent fields retain predictions enabled and steer. Only nondefault preferences
are persisted; no seed revision is required. Publish the integrated commit through
the normal stable workflow. Preserve manual verification limits in the release
record. If release validation fails, fix the source before creating its tag.
