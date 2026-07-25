## 1. Archive persistence

- [x] 1.1 Propagate atomic metadata write success through session storage and
  the internal SessionManager metadata updater.
- [x] 1.2 Add a result-bearing SessionManager operation that archives the
  current session, reports no-active-session separately, and rolls back its
  in-memory flag on persistence failure.

## 2. TUI command lifecycle

- [x] 2.1 Refactor the existing clear/reset lifecycle into one reusable handler
  without changing `/clear` or `/new` behavior.
- [x] 2.2 Register `/archive` and `/archieve`, gate clear on successful archive
  persistence, and update help text and slash-command descriptions.

## 3. Verification

- [x] 3.1 Add focused tests for command registration, successful archive plus
  clear parity, no-active-session behavior, fresh-session archive reset, and
  persistence-failure retention.
- [x] 3.2 Build the TUI and unit-test targets, run focused tests and relevant
  regression tests, run diff/quality checks, and strictly validate the
  OpenSpec change.
