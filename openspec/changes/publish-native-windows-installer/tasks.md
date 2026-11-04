# Tasks

## 1. Package identity

- [x] 1.1 Add Windows setup verification for resource metadata, architecture, payload identity and hashes; cover mismatches with script tests.
- [x] 1.2 Add an isolated x64 install/uninstall smoke check and document its invocation; verify the actual package preserves user data.
- [x] 1.3 Fix the reproduced cross-volume silent-uninstall failure using same-volume retirement, and verify installation and uninstallation with different installation and TEMP drives.

## 2. Release integration

- [x] 2.1 Build and publish both setup architectures in the matrix, include EXEs in checksum collection, and enforce completeness and measured size limits; run release asset tests.
- [x] 2.2 Provide repeatable installer mirroring with preflight hashes and public versioned/alias verification, keeping EXEs outside the updater manifest; test rejected inputs before deployment and document the release command.

## 3. Integration verification

- [x] 3.1 Wait for installer completion, review the stable combined snapshot, and pass applicable static, native and web checks; record evidence and limitations.
- [ ] 3.2 Validate OpenSpec and the final release package's seed upgrade and equal-version managed-hook repair with an isolated profile; record package identity and results.

## Workflow follow-up

- Commit and push all intended ACECode changes, integrate eligible completed worktrees, then publish one stable release and verify all platform downloads plus both Windows installers.
