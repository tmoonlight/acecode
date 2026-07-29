## Context

`SearchPalette` calls `App.handleSelectSession()`. That handler closes the palette immediately, then awaits `resumeAndOpenSession()`. The shared helper does not commit the target `activeRef` until workspace activation and session resume have completed, so the currently mounted home or previous conversation becomes visible during the wait. Cross-workspace jumps can add a second wait after a full-page redirect while the destination parses and resumes its `?open=` target.

The existing resume-before-open ordering is required to avoid opening an unloaded session, and it must not be weakened just to make navigation appear faster. The UI therefore needs an explicit transition state around the existing shared flow.

## Goals / Non-Goals

**Goals:**

- Cover the full viewport from the moment a valid session jump begins until the target session ref is committed or the operation fails.
- Reuse the same state for search, desktop jump entry points, and redirected startup navigation.
- Prevent pointer and keyboard interaction with the intermediate page while navigation is pending.
- Keep failure behavior recoverable by clearing the mask and retaining the existing toast.
- Make the loading state recognizable to assistive technology.

**Non-Goals:**

- Reduce daemon resume latency or change resume-before-open ordering.
- Change search ranking, result rendering, workspace activation, or session persistence.
- Keep the mask visible until every transcript message finishes rendering; the destination chat's existing loading behavior remains responsible after navigation commits.

## Decisions

1. **Own the transition state in `App` around `resumeAndOpenSession`.**

   The shared helper already covers search, startup URL targets, tray/notification bridge calls, and cross-workspace activation. Starting and finishing the state there prevents individual entry points from drifting. Putting state only in `SearchPalette` was rejected because the palette unmounts as soon as it closes and cannot cover redirected startup navigation.

2. **Track active navigation operations and release the mask in `finally`.**

   Each valid jump registers a pending operation before awaiting any workspace or session API. Normal success and handled failure unregister it in `finally`; the mask closes only when no operation remains. This avoids an older overlapping completion hiding the mask for a newer jump.

3. **Treat a full-page URL assignment as a handoff, not a completed navigation.**

   Once a cross-workspace redirect is assigned, the source page keeps its mask until unload instead of briefly revealing the old page. The destination initializes its pending state from the parsed `?open=` target and clears it when startup resume settles.

4. **Render one fixed, top-level themed mask.**

   A focused `SessionNavigationMask` component renders outside the main content shell with a full-viewport fixed layer, opaque-enough themed backdrop, centered animated spinner, and concise `正在打开会话…` label. It uses `role="status"`, `aria-live="polite"`, and `aria-busy="true"` and blocks underlying input while mounted.

## Risks / Trade-offs

- **A redirect never unloads after URL assignment** → The source mask could remain indefinitely. URL construction and assignment stay inside the existing guarded desktop activation path; assignment failures fall back to the current daemon path and normal cleanup.
- **A resume request is very fast** → React may display the mask only briefly or not paint an intermediate frame. No artificial minimum delay is added, avoiding slower navigation solely for animation visibility.
- **A resume request hangs** → The mask remains because the navigation is genuinely pending. Existing API timeout/error handling remains the authority for ending the operation.
- **The destination transcript still needs rendering time** → The full-screen mask ends when `activeRef` commits; the chat view's existing session loading state handles subsequent transcript work.
