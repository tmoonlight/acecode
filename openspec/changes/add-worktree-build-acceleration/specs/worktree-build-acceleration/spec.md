# Spec Delta

## Purpose

Accelerate first development launches in new worktrees without sharing build directories that encode worktree-specific source paths.

## ADDED Requirements

### Requirement: Optional shared compiler cache
The development launcher SHALL detect an available `sccache` executable using the system path and supported platform-specific installation locations. When available, it SHALL configure C and C++ compiler launchers for a development build directory so worktrees may reuse the user's default `sccache` cache. When unavailable or unusable, the launcher SHALL continue with an ordinary local compilation and provide a concise status message with an installation suggestion when the cache is absent.

#### Scenario: Configure an available compiler cache
- **WHEN** a launcher prepares a configured development build and finds a usable `sccache` executable
- **THEN** it configures C and C++ compiler launchers to use `sccache` before the target's incremental build

#### Scenario: Cache tool unavailable
- **WHEN** a launcher cannot find `sccache`
- **THEN** it reports an optional installation suggestion and continues with normal compilation

#### Scenario: Cache tool failure
- **WHEN** compiler-cache configuration, execution, statistics collection, or a cache-enabled build fails
- **THEN** the launcher reports a concise reason, reconfigures without compiler launchers when necessary, and retries ordinary compilation without blocking target startup

### Requirement: Cache configuration transitions
The development launcher SHALL detect when an existing build directory's configured compiler-cache state differs from the currently available cache state. It SHALL reconfigure that build directory before its next incremental build to add or remove compiler launcher settings as required.

#### Scenario: Install cache after initial configuration
- **WHEN** `sccache` becomes available after a build directory was configured without it
- **THEN** the next development launch reconfigures the build directory before building

#### Scenario: Remove cache after configuration
- **WHEN** `sccache` is no longer available for a build directory configured to use it
- **THEN** the next development launch reconfigures the build directory without compiler launcher settings before building

### Requirement: Per-launch compiler-cache status
When `sccache` is used successfully, the development launcher SHALL report a concise summary of the cache hits, misses, and errors observed during that launch's build operation. It SHALL not require cache statistics to start a development target.

#### Scenario: Report build cache activity
- **WHEN** the cache statistics can be read before and after a development build
- **THEN** the launcher reports the per-launch differences for hits, misses, and errors

### Requirement: Verified frontend artifact seed
Before a Web or Desktop development launch builds frontend assets, the launcher SHALL look for a seed `web/dist` from registered Git worktrees. It MAY copy a seed only when the source and current worktrees are at the same commit, both have no uncommitted changes under `web/`, the source has `web/dist/index.html`, and the current output is missing or stale. If more than one eligible source exists, it SHALL select the source with the newest `web/dist/index.html`. It SHALL validate copied output against current frontend input timestamps before skipping the local frontend build.

#### Scenario: Seed missing frontend output
- **WHEN** a new worktree has no `web/dist`, and an eligible same-commit worktree has current frontend output
- **THEN** the launcher copies and validates the newest eligible output, then skips `pnpm build`

#### Scenario: Dirty frontend worktree
- **WHEN** either the source or current worktree has uncommitted changes under `web/`
- **THEN** the launcher does not reuse that source's frontend output

#### Scenario: Seed copy failure
- **WHEN** frontend artifact copying or validation fails
- **THEN** the launcher reports the fallback and runs the ordinary local frontend build
