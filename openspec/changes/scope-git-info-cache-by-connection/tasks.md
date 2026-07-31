## 1. Connection-Scoped Cache Core

- [x] 1.1 Add opaque, dynamically resolved effective-connection scopes to API clients without exposing credential strings.
- [x] 1.2 Implement the shared Git-information cache with per-connection/per-cwd TTL entries, in-flight deduplication, failure eviction, and scoped/global invalidation.

## 2. Consumer Integration

- [x] 2.1 Integrate the reviewed Git session pill visibility guard and route cached loads, forced refreshes, and invalidation through the pill's caller-provided API client.
- [x] 2.2 Route SidePanel and Sidebar Git-information loads through the shared cache with their own API clients.

## 3. Regression Coverage

- [x] 3.1 Add behavioral tests for same-context sharing, TTL reuse, different-origin isolation, different-token isolation, mutable global-base isolation, and invalidation.
- [x] 3.2 Update architecture guards and the Web test entrypoint to enforce visibility short-circuiting and caller API plumbing.

## 4. Validation

- [x] 4.1 Run focused Git-information tests and strict OpenSpec validation.
- [x] 4.2 Run the complete Web test suite, production build, and diff/whitespace checks.
