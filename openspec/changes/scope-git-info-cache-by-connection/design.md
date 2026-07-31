## Context

`/api/git/info` is expensive on Windows because one request starts several Git subprocesses. The existing Web UI can mount `GitSessionPill`, `SidePanel`, and the sidebar hover card for the same directory, so sharing a short-lived result and an in-flight promise is valuable. Claude's proposed singleton achieves that sharing but hardcodes the mutable global API client and keys only by `cwd`, while `ChatView` and `SidePanel` can use session-scoped clients with explicit daemon ports and tokens.

The cache therefore has two independent identity dimensions: the effective connection used to send the request and the repository directory. A workspace hash is not part of the Git endpoint contract; the daemon plus authentication context and `cwd` fully determine the request.

## Goals / Non-Goals

**Goals:**

- Avoid all Git-information traffic when a session pill is known not to render.
- Deduplicate concurrent requests and reuse fresh results across independently created API clients that resolve to the same daemon origin and token.
- Route every cache miss through a client belonging to the requested connection context.
- Prevent cached values and in-flight promises from crossing origin or token boundaries.
- Keep explicit refresh and Git-state invalidation predictable.

**Non-Goals:**

- Changing `/api/git/info`, reducing its daemon-side Git subprocess count, or adding a server cache.
- Changing Git branch/worktree UI behavior.
- Sharing cache state across browser documents or persisting it across reloads.

## Decisions

1. **Expose an opaque effective-connection scope from API clients.** `api.js` will canonicalize the effective origin and token to an opaque object and expose only a helper that returns that object for a client. Independently created clients for the same effective connection therefore share identity, while credentials are not serialized into a public cache key. The scope is resolved when used so the legacy global client follows later `setBase` changes. Using API object identity alone was rejected because `ChatView`, `SidePanel`, and the global sidebar can construct separate clients for the same connection.

2. **Make the Git-information cache accept both the API client and `cwd`.** Entries will be stored per opaque connection scope and then per directory. A cache miss captures the supplied client for that request, ensuring an old scope never reloads through a global client whose base has since changed. The cache retains the existing 30-second TTL, in-flight deduplication, failure eviction, and stale-promise invalidation behavior.

3. **Pass the real caller client at every consumer and gate the pill on transcript readiness.** `GitSessionPill` keeps its `api` prop, `SidePanel` uses its session-scoped client, and `Sidebar` uses the global client. The transcript hook reports `loading` whenever its current state still belongs to the previous session, and the bar pill waits for that load to finish before treating an empty item list as a genuinely new session. This prevents both the normal loading reset and an empty-to-existing session switch from opening a one-render request window. Existing routing remains intact while safe sharing occurs whenever effective connection scopes match.

4. **Use scoped invalidation for consumer reloads and conservative invalidation for unscoped events.** A consumer reacting to a Git-state event invalidates its own connection/cwd before loading. The module-level listener invalidates that cwd in every known scope when the event lacks connection metadata, preventing stale results without choosing a daemon for the reload.

5. **Test runtime behavior, not only source shape.** A dedicated Node test will use independently created API clients to prove same-context in-flight deduplication and TTL reuse, different-origin and different-token isolation for the same `cwd`, and correct invalidation. Lightweight architecture guards remain for the no-render/no-request rule and correct client plumbing.

## Risks / Trade-offs

- **[Connection-scope registry retains a small number of credential identities for the document lifetime]** -> The Web UI normally sees only a handful of daemon connections, stores opaque objects rather than exported credentials, and drops all state on reload.
- **[An unscoped Git-state event invalidates more entries than strictly necessary]** -> Such events are rare and correctness is more important than preserving a 30-second value; reloads still occur only in mounted consumers and use their own clients.
- **[A mutable global API client changes effective scope after `setBase`]** -> Resolve its scope on each cache operation and capture the caller only for the individual cache-miss request.

## Migration Plan

1. Integrate the visibility guard and shared-cache consumers from the reviewed optimization.
2. Replace the global-client singleton with the connection-scoped cache contract and restore caller API plumbing.
3. Add behavioral isolation tests and run the full Web test/build gates.
4. Roll back by reverting the integration commit; no stored data or daemon migration is involved.

## Open Questions

None.
