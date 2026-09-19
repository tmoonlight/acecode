# Proposal

## Why

New Git worktrees require independent CMake build directories for correctness, but compiling every unchanged ACECode source file and rebuilding identical frontend output makes their first development launch unnecessarily slow. The development launcher should safely reuse cross-worktree caches and verified frontend output without sharing path-bound build directories.

## What Changes

- Detect and configure `sccache` as an optional cross-worktree C/C++ compiler cache for development builds.
- Report concise per-launch cache activity and fall back to normal compilation when cache discovery, configuration, or status collection fails.
- Detect cache-tool configuration changes and reconfigure the affected build directory before its next incremental build.
- Seed a new worktree's missing or stale `web/dist` from the newest eligible, clean, same-commit worktree and validate it before skipping Vite.
- Fall back to the existing local frontend build process if frontend artifact reuse is unavailable or fails.

## Capabilities

### New Capabilities

- `worktree-build-acceleration`: Safe cross-worktree compiler-cache and frontend-artifact reuse for development launcher startup.

### Modified Capabilities

- None.

## Impact

- `scripts/dev_environment.py` gains compiler-cache detection, CMake configuration management, and frontend seed selection.
- `scripts/dev_desktop.py` exposes or shares frontend freshness checks used by the launcher.
- Launcher tests cover cache discovery/configuration transitions and frontend-copy eligibility/fallback.
- No production application runtime, daemon API, or shared CMake build directory behavior changes.
