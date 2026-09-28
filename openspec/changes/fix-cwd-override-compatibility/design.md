<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md for the observed failures. The four cwd override entry points now take UTF-8 strings. Old Windows keys used the active filesystem code page before hashing. CORS decoration currently occurs in individual route helpers; Crow's exception callback receives only the response.

## Goals / Non-Goals

**Goals:** Preserve legacy choices, keep canonical settings authoritative, prevent removal from reviving old choices, and retain request context when adding CORS to errors.

**Non-Goals:** Scan or rewrite all project storage, change authentication policy, or introduce frontend changes.

## Decisions

- Perform read-only legacy fallback only when the canonical file is absent. Reproduce the old filesystem conversion inside a guarded Windows-only helper, checking the supplied, native-slash and forward-slash forms and deduplicating resulting paths. Skip failed conversions. This avoids both writes during startup and unbounded project scans. New writes use only the corrected key; explicit removal includes calculated legacy paths.
- Use a Crow response-completion middleware to invoke a shared CORS helper. Existing route wrappers call the same helper; an already decorated response is left alone to prevent duplicate headers. A response-only exception callback cannot safely determine the request Origin, so it remains responsible only for error logging and serialization.

## Risks / Trade-offs

- Legacy settings depend on the active Windows filesystem code page. Compatibility covers upgrades on that code page; settings from an unrelated previous system locale are not guessed.
- Multiple old path spellings can have different legacy files. Prefer the supplied spelling, then native and forward-slash forms; canonical files always win, including malformed canonical files.
- Middleware changes affect all routes. Verify the complete HTTP suite, especially cross-origin authorization, preflights, WebSocket-related HTTP routes, and global errors.

## Migration Plan

Deploy the UTF-8 correction with the compatibility fallback. Existing legacy files are retained until explicit removal; saving a model writes a canonical setting. Merge the verified branch into local master while preserving unrelated working-copy changes.
