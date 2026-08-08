## 1. Composer Selection Lifecycle

- [x] 1.1 Separate one-shot submission cleanup from conversation-context selection reset in `ChatView`.
- [x] 1.2 Preserve swarm mode through normal, queued, and home-to-session sends while retaining the existing expert binding and restoration path.

## 2. Regression Coverage and Validation

- [x] 2.1 Update focused WebUI regression tests to enforce persistent swarm and expert selections after submission.
- [x] 2.2 Run focused/full WebUI tests, the production WebUI build, strict OpenSpec validation, and diff checks.
