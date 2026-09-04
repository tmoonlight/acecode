## 1. Git branch metadata

- [x] 1.1 Extend `GitInfo` collection to distinguish local branches from non-symbolic remote-tracking branches without network access.
- [x] 1.2 Serialize `remote_branches` from `/api/git/info` and document the additive response field.
- [x] 1.3 Add C++ regression coverage for local/remote classification, symbolic remote exclusion, and API payload serialization.

## 2. Side-panel comparison selector

- [x] 2.1 Update the pure candidate builder to order the verified default remote base, remaining remote refs, local refs, and `HEAD` with stable deduplication and compatibility fallbacks.
- [x] 2.2 Add Web regression coverage for remote-first ordering, local choices, missing fields, malformed entries, and candidate bounds.
- [x] 2.3 Verify the panel keeps branch selection read-only and carries the selected base into list and detail comparison flows.

## 3. Validation

- [x] 3.1 Run focused C++ and Web tests for Git metadata and selector behavior.
- [x] 3.2 Run the full Web test suite, production Web build, strict OpenSpec validation, and `git diff --check`.
