## Context

`/api/git/info` currently returns the current branch, a verified `default_base`, and local branches. `GitChangesPanel` ignores the local branch array and builds its selector from only `default_base` and `HEAD`; an unused helper argument anticipated remote refs but no backend field supplies them. The existing `/api/git/changes` and `/api/git/diff` endpoints already accept any verified safe ref, so the missing capability is branch discovery and selector construction rather than a new comparison endpoint.

The panel is a read-only review surface. Changing its selection must not reuse the checkout endpoint or mutate the repository. Remote discovery must inspect fetched remote-tracking refs only and must not perform network I/O.

## Goals / Non-Goals

**Goals:**

- Expose safe, locally available remote-tracking branch names alongside the existing local branch list.
- Prefer the verified remote default as the initial comparison base.
- Offer other remote and local branches as comparison bases, with deterministic ordering and no duplicates.
- Preserve `HEAD` as the compatibility and no-remote fallback.
- Keep list and detail views on the same selected comparison base.

**Non-Goals:**

- Fetching, pruning, creating, deleting, or checking out branches.
- Changing the worktree branch selector used when creating a new session.
- Adding arbitrary user-entered refs, tags, or commit hashes.

## Decisions

### Add a separate `remote_branches` metadata field

The collector will continue returning local short names in `branches` and add remote-tracking short names in `remote_branches`. A single `for-each-ref` invocation will inspect `refs/heads` and `refs/remotes`; symbolic refs such as `origin/HEAD` will be excluded and every emitted short name will pass the existing safe-ref validator.

Keeping separate arrays lets the Web layer order remote choices ahead of local choices without guessing from a slash in the name. Preserving `branches` avoids breaking existing clients. Reusing `branches` for both kinds was rejected because local branch names may contain slashes and cannot be reliably classified afterward.

### Build one ordered, deduplicated candidate list in the pure Web helper

`buildBaseCandidates` will consume the Git info payload directly. It will place the verified `default_base` first, then other remote refs, local refs, and `HEAD`, deduplicating while preserving order. The initial selection remains `default_base` when present and `HEAD` otherwise, so a pure-local repository still defaults to its current checkout state rather than an arbitrary alphabetically first local branch.

Candidate construction remains in the pure helper so ordering, caps, compatibility with older payloads, and malformed entries are testable without React.

### Reuse the existing read-only comparison flow

Selecting a candidate will continue updating the panel's `base` state and calling `/api/git/changes`; opening a file carries that same base into `GitChangeReview` and `/api/git/diff`. No checkout call or Git state-change event will be introduced.

## Risks / Trade-offs

- **[Risk] Repositories with thousands of refs could create an unwieldy menu or response.** -> Bound the displayed remote and local candidates independently while always retaining the verified default and `HEAD` fallback.
- **[Risk] A remote symbolic alias could appear as a selectable branch.** -> Inspect the symbolic-ref field and omit symbolic entries such as `origin/HEAD` in the collector.
- **[Risk] Old daemons do not return `remote_branches`.** -> Treat missing arrays as empty and preserve the existing verified-default plus `HEAD` behavior.
- **[Risk] Stale remote-tracking refs may be shown.** -> Deliberately show locally fetched refs only; manual/existing Git refresh invalidation remains authoritative and no hidden network fetch is performed.
