## 1. Contract And Catalog

- [ ] 1.1 Add pure parsing, formatting, ranking, chunking, and numbered-snapshot tests for the three aliases
- [ ] 1.2 Build a generic all-project session catalog that excludes archived and child sessions and merges active state
- [ ] 1.3 Reuse the user-message index for optional content matches while preserving metadata-only fallback

## 2. Binder Integration

- [ ] 2.1 Intercept channel-side session commands before normal `send_input`
- [ ] 2.2 Implement lifecycle-safe numeric selection and exact workspace/no-workspace resume
- [ ] 2.3 Preserve generation, question bridge, acknowledgement, keepalive, persistence, and shutdown invariants
- [ ] 2.4 Keep every session catalog/search/select operation off the RC HTTP inbound callback via the owned control worker

## 3. Frontend Navigation

- [ ] 3.1 Broadcast a generic successful-selection WebSocket event without exposing remote-control secrets
- [ ] 3.2 Navigate through the existing resume/open helper and refresh the session list
- [ ] 3.3 Force the existing lightning surge on the selected row, including newly mounted cross-workspace rows and reduced-motion behavior

## 4. Verification

- [ ] 4.1 Add C++ focused tests for aliases, default/all/search, stable numbering, errors, inactive cross-workspace resume, concurrent shutdown/rebind, and a stalled catalog dependency that does not block inbound
- [ ] 4.2 Add Node tests for event normalization, navigation target construction, and forced surge behavior
- [ ] 4.3 Run focused C++ tests, Web tests/build, feasible full C++ suite, OpenSpec validation, and provider-string boundary checks
- [ ] 4.4 Update durable remote-control documentation, complete checkboxes, and commit the scoped change
