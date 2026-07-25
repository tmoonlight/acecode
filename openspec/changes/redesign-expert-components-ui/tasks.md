## 1. Expert schema and lossless persistence

- [x] 1.1 Extend expert definitions and drafts with author, Tags, expertise, timestamps, and independently optional Skill/MCP/tool capability scopes.
- [x] 1.2 Parse, validate, normalize, and serialize missing, empty, non-empty, Unicode, duplicate, and unavailable capability/list values with focused registry tests.
- [x] 1.3 Make expert updates merge managed fields into the existing manifest while preserving avatar settings, packaged Skills, resources, and unknown fields; cover preservation and workspace read-only behavior with tests.
- [x] 1.4 Extend expert list/create/update HTTP DTOs and smoke tests for the new expert and team fields without exposing package-private data.

## 2. Runtime capability catalog and enforcement

- [x] 2.1 Add exact tool-source ownership metadata and a sanitized runtime capability catalog for Skills, MCP server IDs/status, and registered built-in tool IDs.
- [x] 2.2 Add the read-only expert capability-catalog API, ensure MCP command/env/header/token data is excluded, and cover availability/unavailable states with HTTP tests.
- [x] 2.3 Intersect optional expert Skill selections with global allowed/disabled policy while retaining valid packaged Skills across create, resume, fork, and switch paths.
- [x] 2.4 Add session-local built-in/MCP tool filtering to provider schemas and an execution-boundary denial check without unregistering global tools or bypassing permissions.
- [x] 2.5 Atomically update prompt context, Skill registry, MCP scope, and built-in-tool scope in the queued expert switch; preserve member-specific scopes for expert-team delegation.
- [x] 2.6 Add focused C++ tests for schema filtering, execution denial, global-policy intersection, member isolation, and in-flight-turn switch ordering.
- [x] 2.7 Document the extended expert and capability-catalog API in `docs/daemon-api.md`.

## 3. Frontend data and API contracts

- [x] 3.1 Extend expert normalization and form payload helpers for author, Tags, expertise, timestamps, and optional capability scopes without mapping opening prompts onto cards.
- [x] 3.2 Implement and test dynamic Tag collection, multi-Tag membership, combined search, recent-use/recent-create sorting, line-list parsing, and team lead/member validation.
- [x] 3.3 Add the sanitized capability-catalog API client and frontend state helpers for available, globally disabled, disconnected, and saved-but-missing choices.
- [x] 3.4 Replace architecture tests that encode the obsolete “no Skill/MCP editor” and “dispatch back to a new task” contracts.

## 4. Expert catalog, cards, and detail

- [x] 4.1 Rebuild the expert-components page header, primary expert/team tabs, horizontally scrollable dynamic Tag rail, search, real sorting, result summary, and management actions using ACECode tokens.
- [x] 4.2 Implement responsive expert and team cards with real avatar fallback, author, Tags, expertise, team lead/member count, semantic detail activation, and a non-bubbling `派遣` action.
- [x] 4.3 Implement accessible expert/team detail dialogs with expertise, team members, editable/read-only state, opening prompts, dispatch actions, focus restoration, and no page-level chat input.
- [x] 4.4 Add skeleton, filter-empty, catalog-empty, local error/retry, unavailable, light/dark/orange theme, and narrow-width states.

## 5. Expert and team editors

- [x] 5.1 Build one expert editor with `基础信息` and `高级功能` tabs, preserved unsaved state, field-level validation, busy/error handling, and unsaved-close confirmation.
- [x] 5.2 Implement the basic fields including separate Tag multi-select, one-line-per-item expertise and opening-prompt editors, introduction, author/call name, and work instructions.
- [x] 5.3 Implement searchable Skill and MCP selectors plus Toggle-based real built-in-tool controls, selection counts, unavailable reasons, and exact optional-scope semantics.
- [x] 5.4 Replace the full-page team picker with an in-editor searchable/Tag-filtered member picker and rows that support add, remove, and exactly one lead.
- [x] 5.5 Wire create, update, confirmed delete, in-place refresh, managed-global edit boundaries, and accessible responsive dialog behavior.

## 6. Conversation-hosted dispatch experience

- [x] 6.1 Reuse the expert catalog/detail as a `ChatView`-hosted picker for `更多专家`, preserving the current session, transcript, draft, attachments, route, and composer focus.
- [x] 6.2 Make opening prompts dispatch the selected expert and fill the existing composer draft without auto-send; keep the picker and old draft/expert coherent on failure.
- [x] 6.3 Update the shared plus-menu recent rows to show one-line expertise/type summaries, render no recent container or separator when empty, keep the combined five-item limit, and flip placement when space is constrained.
- [x] 6.4 Update the composer expert status to show expert/team type and a busy-session `下一轮` state, promoting it only after the queued switch boundary.
- [x] 6.5 Provide an explicit target-conversation chooser for dispatch from the standalone management route when no conversation dispatch context exists, without creating a fake composer or silent new task.

## 7. Verification and delivery

- [ ] 7.1 Run all Web unit tests and production build, fixing regressions and keeping generated output out of direct edits.
- [ ] 7.2 Build and run focused expert, Skill, MCP, AgentLoop/tool-policy, prompt, and Web HTTP C++ tests.
- [ ] 7.3 Run the available full C++ test suite and distinguish environment-only failures from product regressions.
- [ ] 7.4 Validate the OpenSpec change strictly and run `git diff --check`.
- [ ] 7.5 Perform real-browser interaction and screenshot review at wide, medium, and narrow widths across light, dark, and orange themes, including every empty/error/pending state and confirming the expert page has no bottom chatbox.
