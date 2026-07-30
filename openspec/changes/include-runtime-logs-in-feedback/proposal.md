## Why

Desktop/Web feedback currently uploads only the desktop-shell log, so it omits
the daemon log that contains the session, provider, tool, and request handling
activity needed to diagnose most failures. Browser-only deployments therefore
upload no runtime log at all.

## What Changes

- Let feedback packages contain multiple independently bounded log tails.
- Include the newest available desktop and daemon rotated logs in Desktop/Web
  feedback, while continuing to allow daemon-only and log-missing submissions.
- Include the TUI working-directory log plus available desktop/daemon runtime
  logs in `/feedback`.
- Report each requested/included log in the API response and `feedback.json`,
  while preserving the aggregate log availability and byte-count fields.
- Document and test multi-log, partial-availability, entry-name collision, and
  rotated-log selection behavior.

## Capabilities

### New Capabilities

- `runtime-feedback-logs`: Defines bounded multi-runtime log selection,
  packaging, metadata, and caller-specific feedback scope.

### Modified Capabilities

None.

## Impact

- Feedback package types and helpers in `src/feedback/feedback_upload.*`.
- TUI `/feedback` and `POST /api/feedback/desktop` callers.
- Desktop feedback API response/metadata and `docs/daemon-api.md`.
- Focused C++ package and Web route tests plus feedback UI wording/i18n.
