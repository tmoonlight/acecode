## Context

App owns navigation and panels; ChatView owns input, cancellation and reasoning changes. Existing Ctrl+K and Ctrl+backquote listeners and xterm forwarding are separate. Browser-default guards prevent Ctrl+- before bubble listeners. ChannelsSettings already maps enabled channels to disconnect, but status, management and pending requests add conditional rows.

## Goals / Non-Goals

Keep each action's existing handler and lifetime owner. Share shortcut definitions between dispatch, help and terminal forwarding. No backend protocol change, packaging or release.

## Decisions

1. A pure shortcut catalog defines exact modifiers, platform labels and scope; a shared hook handles IME, repeats, consumed events and overlay checks. App owns global actions; ChatView owns conversation actions. Replace the existing global listeners to prevent duplicate dispatch. The zoom guard still prevents native zoom for navigation keys, and marks those events with a shared symbol so the application handler can consume them. This also suppresses zoom when a dialog blocks navigation. Let xterm forward app shortcuts without sending them to the shell.
2. Defaults: Ctrl+Alt+N new session, Ctrl+Alt+I focus input, Ctrl+Alt+B project sidebar, Ctrl+Alt+E right panel, Ctrl+, settings, Ctrl+/ shortcuts, Ctrl+Shift+. stop; Ctrl+- forward and Ctrl+Shift+- back. macOS uses Cmd/Option except the existing Ctrl+backquote console key. Reasoning accepts Ctrl/Cmd+Alt+Period/Comma with or without Shift, covering both physical punctuation keys and literal >/<. No wrapping; order comes from canonical reasoning efforts and only supported levels participate. Effective default depth determines the initial step.
3. Scoped actions do not act behind dialogs, menus, question/permission controls, file editors or a side chat. Reasoning uses the existing async mutation and in-flight guard, including during busy turns. Navigation retains unsaved-file confirmation. New session uses the existing current-workspace action and prevents repeat creation.
4. Settings section follows existing navigation/search/i18n patterns, with the archived section's sticky offsets and opaque surface. The searchable catalog includes existing Enter, Shift+Enter, history, Escape, save, find, search and console shortcuts with scope explanations.
5. Channel cards keep a single stable header and description; management sits before a constant-width connection control. Status is an accessible dot; pending count is attached to management. Full status, recovery and approvals move into ManageDialog. No conditional rows on cards. Narrow layouts use one column.
6. Bundle upstream SVG brand assets locally and document version/source/license; use actual vector shapes with theme-safe rendering, no runtime CDN dependency.

## Risks / Trade-offs

- Browser shortcuts and punctuation layout differences → exact modifier matching, physical-key fallbacks, IME/AltGraph guards, browser regression coverage.
- Hidden/multiple ChatViews → bind conversation shortcuts only to the visible active main composer; do not leak to side chats.
- Reduced status text → accessible status titles and discoverable management details preserve recovery and approval operations.
- Existing channel change said generic monochrome chat icons → this user's explicit brand-icon request supersedes that choice.

## Migration Plan

Ship with the regular Web build; Desktop requires a later embedded bundle rebuild. No stored-data migration. Reverting this change restores the old UI and listeners.

## 2026-10-11: Editable bindings

- Every displayed binding gains an accessible edit icon to its right. A focused recording dialog captures a chord without dispatching it; Save validates conflicts and reserved keys, Cancel leaves the current binding intact, and Restore default resets only the selected action. History exposes separate previous/next bindings within its existing row.
- Keep defaults immutable and persist validated overrides through the existing preference storage helpers. One observable store supplies dispatch, help, button hints and terminal forwarding; updates apply immediately and storage events synchronize other windows. A failed persistent write leaves the old binding active and reports an error.
- Contextual send/newline/history/save/find/dismiss handlers consult the same registry while retaining their current focus and IME guards. Do not synthesize keyboard events or create parallel action implementations. Browser-default suppression and sandboxed presentation forwarding follow the effective binding.
- Record exact physical key and modifiers. Prevent duplicates (including optional-Shift reasoning defaults), standalone modifiers, printable unmodified global shortcuts, focus traversal and known system/browser editing chords. Preserve visible close/cancel controls so recording Escape is safe.
