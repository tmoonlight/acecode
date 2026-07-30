## 1. Multi-Log Package Core

- [x] 1.1 Replace the single optional package log with source/inclusion lists, per-source caps, aggregate metadata, and collision-safe zip entries.
- [x] 1.2 Add reusable newest-rotation discovery for desktop and daemon runtime logs with stable archive names.

## 2. Feedback Caller Integration

- [x] 2.1 Wire Desktop/Web feedback to available runtime logs and TUI `/feedback` to its working-directory log plus available runtime logs.
- [x] 2.2 Return per-log API metadata, update feedback UI/i18n wording, and document the revised daemon API contract.

## 3. Tests And Validation

- [x] 3.1 Add focused package and Web route coverage for multiple logs, partial availability, per-source caps, collisions, and daemon-only submissions.
- [x] 3.2 Build and run the focused C++ feedback and Web route tests.
- [x] 3.3 Run the Web test suite and production build.
- [x] 3.4 Run strict OpenSpec validation and Git diff checks.
