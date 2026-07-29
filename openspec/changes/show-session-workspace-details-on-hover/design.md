## Context

`SessionRow` is the shared desktop sidebar renderer for workspace-grouped and pinned sessions, and it is also reused for the separate no-workspace task section. Workspace session normalization already supplies the effective `cwd`; no-workspace normalization deliberately clears it. The sidebar is an overflow-clipped, compact column, so an in-row detail element would either be clipped or change row geometry.

ACECode already exposes the read-only `/api/git/info?cwd=...` endpoint. It returns `is_repo: false` for non-Git directories and supplies the current branch for repositories, with the existing workspace path allowlist and Git timeout protections.

## Goals / Non-Goals

**Goals:**

- Reveal the exact effective working directory for workspace-backed session rows without changing their compact layout.
- Add the current branch only when the directory is a Git repository.
- Apply the same behavior to pinned and workspace-grouped instances of `SessionRow`.
- Keep no-workspace session rows free of both the detail card and Git metadata requests.
- Preserve hover behavior in the overflow-clipped sidebar and keep the card within the viewport.

**Non-Goals:**

- Changing workspace/session persistence or the session-list API shape.
- Exposing a directory for no-workspace sessions.
- Adding branch switching or any other Git mutation to the hover card.
- Changing session selection, rename, pin, archive, attention, or remote-control behavior.

## Decisions

### Use the existing session `cwd` and lazy Git lookup

The card renders `cwd` immediately from the normalized session object. When the card first opens, it requests `/api/git/info` for that directory and conditionally adds the returned branch. Git lookup remains best-effort: non-repository and failed responses omit only the branch row.

This avoids adding Git subprocess work to every session-list refresh. Enriching the session-list response was considered, but it would eagerly query Git for sessions the user may never inspect and couple navigation serialization to repository state.

### Cache Git information by directory for a short bounded interval

Hovering multiple rows that share a directory should not repeat the same Git query. A small shared cache deduplicates in-flight requests and reuses successful responses for a named, bounded TTL; failed requests are not cached so a later hover can retry. Existing Git-state change events invalidate matching cached entries.

Per-row permanent caching was considered but would leave branches stale after a checkout and duplicate requests between pinned and grouped copies of the same session.

### Render a non-interactive portal tooltip

The detail card is portaled to `document.body`, positioned beside the session row, and clamped to viewport margins after measuring its rendered size. It uses `role="tooltip"`, theme tokens, and pointer-events disabled so it cannot intercept row actions. Pointer hover is the primary trigger; focus entering the row exposes the same information for keyboard users.

Rendering inside the row was rejected because the sidebar clips overflow and the card must not consume row space. A native `title` attribute was rejected because it cannot reliably present two labeled rows, use theme styling, or expose predictable testable behavior.

### Keep eligibility in a pure presentation model

A small pure helper derives whether details exist and which values may render. It treats either no-workspace marker as authoritative and also requires a non-empty `cwd`. This provides focused regression coverage for the privacy boundary independently of React rendering.

## Risks / Trade-offs

- **Git state can change outside ACECode between cache refreshes** -> Keep the cache TTL short and invalidate it on ACECode's existing Git-state event.
- **A slow Git command can delay the branch row** -> Render the working directory immediately and add no loading placeholder; timeout or failure leaves the directory-only card usable.
- **Long paths can produce a tall card near viewport edges** -> Measure the portal content and clamp both axes, while allowing the path to wrap.
- **Pinned and grouped copies can coexist** -> Cache by `cwd`, not component identity, so both copies share the same result.

## Migration Plan

This is an additive frontend-only interaction. Deploy the new helper, tooltip component behavior, and styles with the existing WebUI bundle. Rollback consists of removing those frontend additions; no stored data or API migration is involved.

## Open Questions

None.
