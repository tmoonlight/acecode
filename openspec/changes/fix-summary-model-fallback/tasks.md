# Tasks

## 1. Model recovery

- [x] 1.1 Implement persistent successful-conversation identities and shared fallback executor; test ordering, persistence, deduplication, changed configuration, cancellation and exhaustion.
- [x] 1.2 Connect immutable provider identity to successful conversation requests; test success versus incomplete, empty, failed and cancelled streams.

## 2. Background consumers and settings

- [x] 2.1 Integrate daemon/TUI title, home intent and memory generation; test failed preferred model and successful backup with existing behavior regressions.
- [x] 2.2 Allow automatic summary selection and remove missing-model prompts; update API docs and settings tests, verify autosave in the complete settings page.

## 3. Integration verification

- [x] 3.1 Build native test target and run affected regressions, run web tests/build, validate OpenSpec and review scoped diff; record evidence and limitations in design.md.
