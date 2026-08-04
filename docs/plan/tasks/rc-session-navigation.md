# RC Session Navigation

- Source of truth: `openspec/changes/add-rc-session-navigation/`
- Delivery branch: `task/acecode-rc-session-navigation`
- Isolation: `N:\Users\shao\.worktrees\acecode-rc-session-navigation`

## Develop

- Implement the OpenSpec tasks in order.
- Keep ACECode core free of provider- and company-specific identifiers.
- Preserve the current one-session binding, generation fence, question bridge, immediate inbound acknowledgement, keepalive, and config persistence.
- Queue every channel-side session command onto the binder-owned control worker; filesystem scans, index refreshes, and selection must never block the RC HTTP inbound callback.

## Verify

- Independent review must inspect command routing, snapshot stability, cross-workspace/no-workspace resume, replacement rollback, callback lifetime, and frontend event trust boundaries.
- Required gates: focused C++ tests, Web Node tests and production build, OpenSpec strict validation, provider-string boundary test, and the feasible full unit suite.
