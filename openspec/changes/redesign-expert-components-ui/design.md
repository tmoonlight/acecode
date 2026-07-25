## Context

ACECode already has an expert package registry, referenced expert teams, session persistence, a shared composer plus menu, a persisted five-item recent list, and worker-serialized in-place expert switching. The existing catalog and editor, however, expose only a small subset of the package model, render `quick_prompts` as if they were expertise, route “more experts” and catalog dispatch through a new-task screen, and have no per-expert MCP or built-in-tool isolation.

The implementation must reconcile three sources without copying a prototype wholesale:

1. `design-prototypes/expert-components/DESIGN_REQUIREMENTS.md` is the behavioral and semantic contract.
2. The imported `ACECode Web UI -experts view-.dc.html` is the primary catalog/detail/editor visual reference, and `acecode-app.jsx` is the composer-menu/status reference.
3. Existing production state, APIs, permission checks, themes, and session queue semantics remain architectural boundaries.

The imported static Skill/MCP/tool arrays, hard-coded avatar colors, Design Canvas runtime elements, old “dispatch to a new task” navigation, and obsolete bottom-chat prototype are reference-only and MUST NOT enter production.

## Goals / Non-Goals

**Goals:**

- Deliver the imported expert catalog, detail, editor, team editor, menu, and status experience using ACECode’s production tokens and components.
- Make Tags, expertise, and opening prompts first-class, separate data.
- Preserve the current conversation, draft, attachments, and transcript when opening the picker, selecting a prompt, or switching an expert.
- Persist and enforce optional per-expert Skill, MCP-server, and built-in-tool scopes without broadening global policy.
- Preserve legacy expert behavior and package resources while adding the new schema.
- Provide deterministic loading, empty, error, unavailable, busy, keyboard, theme, and responsive states.

**Non-Goals:**

- Replacing the existing conversation composer, session persistence format, global Skill/MCP settings, permission manager, or sandbox.
- Letting an expert install a Skill, configure MCP credentials, start a globally disabled server, or grant itself permission.
- Copying prototype seed data, a second icon set, hard-coded palette values, or the Design Canvas runtime.
- Adding a chat input, fake status bar, or floating composer to the standalone expert-components page.
- Automatically sending an opening prompt.

## Decisions

### 1. One catalog component, two hosting contexts

The catalog will be decomposed into reusable discovery, card, detail, editor, and team-member components:

- The sidebar route hosts the full standalone management page.
- `ChatView` hosts the same discovery/detail experience as a modal or drawer when the user chooses `更多专家`.

The conversation-hosted variant receives an explicit dispatch context containing the session/new-task binding callback, current draft setter, attachment-preserving composer state, and focus-return target. It never navigates or unmounts `ChatView`. The standalone route does not manufacture a composer; when no dispatch target exists it opens an explicit recent-conversation target chooser rather than silently creating a new task.

This is preferred to routing away and reconstructing draft state because draft text, attachments, optimistic expert state, and focus already live in `ChatView`.

### 2. Production information model mirrors the content semantics

Agent manifests gain managed fields equivalent to:

```json
{
  "author": "吴八哥",
  "tags": ["OPC-一人公司", "开发"],
  "expertise": ["高级开发", "架构设计", "代码质量"],
  "quick_prompts": ["审查当前改动"],
  "created_at": "2026-07-25T00:00:00Z",
  "updated_at": "2026-07-25T00:00:00Z",
  "capabilities": {
    "skills": ["frontend-design"],
    "mcp_servers": ["github"],
    "tools": ["file_read", "AskUserQuestion"]
  }
}
```

`quick_prompts` remains the persisted opening-prompt field for compatibility. `expertise` is new and is the only list used as card capability content. `tags` is an ordered, de-duplicated list and never an exclusive category. `profession`, `description`, and `instructions` keep their existing meanings. Teams may have author, Tags, description, expertise, and timestamps, but their runtime capabilities come from referenced member experts rather than a copied union.

The manifest’s existing top-level `skills` value continues to mean packaged Skill directories. Selected installed Skill names live under `capabilities.skills`, avoiding a schema collision.

On update, the registry reads the existing manifest object, overwrites only ACECode-managed fields, and writes it atomically. It preserves unknown keys, avatar configuration, packaged Skill directories, and all package files. Workspace packages remain read-only through the global CRUD API.

### 3. Optional scopes distinguish inheritance from an explicit empty set

Each property inside `capabilities` is independently optional:

- missing property: inherit all capabilities currently allowed globally;
- present empty array: allow none in that class;
- present non-empty array: allow the named subset.

C++ uses optional collections rather than plain vectors so these states survive parse/serialize round trips. Unknown saved identifiers are retained and returned as unavailable choices instead of being deleted during unrelated edits. Legacy manifests without `capabilities` therefore keep current behavior.

### 4. A single sanitized capability-catalog API backs the advanced tab

A read-only expert capability endpoint will assemble current-context choices from production registries, for example:

```json
{
  "skills": [
    {"id":"frontend-design","description":"…","source":"global","available":true}
  ],
  "mcp_servers": [
    {"id":"github","transport":"stdio","status":"connected","available":true}
  ],
  "tools": [
    {"id":"file_write","description":"…","available":true,"configurable":true}
  ]
}
```

The endpoint reuses Skill discovery, `McpManager` status/tool ownership, and the live `ToolExecutor` registry. It never returns MCP command arguments, environment values, headers, auth tokens, or credentials. IDs are the exact runtime Skill name, MCP server key, and registered tool name; the frontend owns no parallel capability list. Saved-but-missing identifiers are merged into the editor model with an unavailable reason.

This dedicated DTO is preferred to composing `/api/skills` with the raw `/api/mcp` configuration because the latter exposes configuration details irrelevant to expert authoring and cannot reliably describe live tool ownership.

### 5. Tool visibility is session-local and enforced twice

Disabling an MCP server or unregistering a tool globally is not acceptable because the daemon’s `ToolExecutor` is shared by concurrent sessions. Instead:

- `ToolImpl`/tool catalog records its source and, for MCP tools, the exact owning server ID.
- `AgentLoop` owns an immutable-at-turn-boundary expert capability policy.
- API request construction filters provider-facing definitions by that policy and global availability.
- Execution checks the same policy again before permission prompting or implementation dispatch and returns a deterministic denied result for a hidden tool.

Filtering uses MCP ownership metadata supplied by `McpManager`, not a guessed name prefix. Built-in tools use their exact registered IDs, including casing. Existing permission, question, dangerous/plan mode, path, and sandbox checks remain downstream and unchanged.

### 6. Skill scope is applied by registry intersection

`initialize_skill_registry` will accept an optional expert Skill-name scope and intersect it with `SkillsConfig::allowed` and the global disabled list through the existing `SkillRegistry::set_allowed` semantics. Valid packaged Skill roots remain scan roots for that expert and can be identified as package-provided; global/project Skills outside an explicit expert scope are not indexed, injected, or returned by Skill tools.

The same helper is used for create, resume, fork, member context, and active-session switch so behavior cannot drift between entry paths.

### 7. Expert switching updates prompt and capabilities atomically

`SessionRegistry::switch_expert` resolves the new definition and builds its Skill registry and tool policy before enqueueing a control operation. Inside that one operation it updates:

- persisted expert binding;
- expert/member prompt context;
- session Skill registry;
- MCP server scope;
- built-in-tool scope.

Because the control is serialized with turns, an in-flight turn retains its old prompt and tool policy. The UI shows a `下一轮：{专家名}` pending state while the session is busy and promotes it to the current `已派遣` state after the control boundary/session-status confirmation. Failed API calls roll back the optimistic selection.

Referenced teams expose a member-aware capability selection helper. Delegated/member sessions use the selected member’s policy, never a union of all members or the lead’s broader policy.

### 8. Opening prompts write through the real composer contract

Selecting an opening prompt performs two explicit actions in order: dispatch the expert in the current conversation context, then set the existing composer draft. It does not call the send path. The picker closes only after the binding is accepted (or locally staged for a not-yet-created conversation), focus returns to the composer, and current attachments remain untouched.

If dispatch fails, the old expert remains selected, the picker/detail stays available, and the draft is not silently replaced.

### 9. Visual implementation adopts density, not prototype plumbing

The page follows the imported proportions while using only ACECode tokens/classes:

- centered content up to about `1240px`, `24px 32px 56px` page spacing;
- primary tabs around `34px`, horizontally scrollable `26px` Tag pills;
- responsive `repeat(auto-fill, minmax(310px, 1fr))`-equivalent card grid with `14px` gaps;
- cards around `15px` padding, `12px` radius, token border, 36px avatar, 26px dispatch action;
- detail dialog about `700px`, editor about `940px`, both height-bounded with scrolling body and always-visible footer;
- desktop two-column basic/advanced sections collapse to one column on narrow screens;
- detail/editor become near-full-screen on narrow devices;
- at most three expertise chips on cards; real avatar first, token-colored initials only as fallback.

Existing `VsIcon` assets and `globals.css` semantic tokens cover icons, light/dark/orange themes, focus, borders, backgrounds, and state colors. The imported hard-coded colors, shadows, and duplicate SVG files are not copied. Local tools use the production `Toggle`; Skill/MCP selections use checkbox/list semantics.

### 10. Interaction and state behavior is explicit

- Cards have a semantic detail activation target; the nested `派遣` button stops detail activation.
- Dialogs trap focus, close on Escape/overlay when not busy, restore focus, and confirm unsaved dismissal.
- Loading uses Tag and card skeletons, not a page-blocking spinner.
- Empty filters offer clear/reset; load failure offers local retry.
- Read-only workspace experts do not expose a misleading enabled edit action.
- Recent-menu separators render only when recent items exist; the menu flips to fit viewport space.
- Create/update/delete refresh catalog data in place and deletion remains explicitly confirmed.

## Risks / Trade-offs

- **[Cross-cutting runtime filtering can create schema/execution drift]** → Centralize the predicate in one capability-policy helper and test both definition filtering and execution denial with identical fixtures.
- **[Shared MCP registration can change while a turn is active]** → Snapshot available definitions at request construction, keep the expert allowlist stable for that turn, and let normal unknown/unavailable handling reject a tool removed globally before execution.
- **[Legacy and explicit-empty scopes are easy to conflate]** → Model each collection as `optional`, add JSON round-trip tests for missing/empty/non-empty, and never default an editor-loaded legacy scope to an explicit empty array unless the user changes it.
- **[Updating manifests may destroy third-party extensions]** → Merge managed fields into the original JSON object, use atomic replace, and regression-test unknown keys plus avatar/Skill/resource files.
- **[Conversation picker reuse can leak standalone navigation assumptions]** → Pass an explicit dispatch context and test that session ID, draft, attachments, and route remain unchanged.
- **[Exact MCP ownership is unavailable in the current ToolExecutor model]** → Add source-owner metadata at registration and expose a read-only catalog; do not infer ownership from qualified-name prefixes.
- **[Pending “next turn” UI can race session status]** → Use the server response plus busy/session events as the source of truth and keep a separate pending binding until the worker boundary is observed.
- **[Large editor dialogs can regress at 900px and mobile widths]** → Bound header/footer, scroll only the body, collapse grids under the existing breakpoint, and include browser screenshots at 1440px, about 900px, and 375/640px.

## Migration Plan

1. Extend parsing/serialization and DTOs with optional new fields while keeping all legacy defaults unchanged.
2. Add lossless manifest update tests before changing the editor payload.
3. Add the sanitized capability catalog and session-local filtering behind absent-scope compatibility.
4. Update pure frontend normalization, filtering, sorting, and form payload helpers with tests.
5. Replace catalog/detail/editor/team UI and then reuse the catalog as the conversation-hosted picker.
6. Update the composer recent row/status behavior without changing the existing five-item storage key.
7. Run strict OpenSpec validation, focused C++ tests, the full Web suite/build, and browser visual/interaction checks across themes and widths.

Rollback is a normal source rollback: legacy manifests remain readable because the new fields are optional. Manifests saved with new fields remain parseable by older versions because existing parsers ignore unknown keys, though older runtimes will not enforce the scopes; this limitation must be noted if shipping versions can be downgraded.

## Open Questions

No product-blocking questions remain. The implementation will use an explicit recent-conversation target chooser when the standalone management route lacks a dispatch context, retain exact runtime identifiers in advanced configuration, and treat the requirements document—not prototype demo behavior—as authoritative where sources conflict.
