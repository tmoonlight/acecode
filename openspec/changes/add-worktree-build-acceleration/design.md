# Design

## Context

See `proposal.md` for motivation and `specs/worktree-build-acceleration/spec.md` for requirements. `scripts/dev_environment.py` currently validates builds against Git worktrees, keeps build directories independent, incrementally builds the selected target, and calls the Desktop launcher module to refresh frontend assets. Its first-worktree build compiles all ACECode sources even when a related worktree has already compiled identical content.

## Goals / Non-Goals

**Goals:**

- Preserve one CMake/Ninja build directory per worktree and selected preset.
- Reuse content-addressed compiler results through the user's existing `sccache` cache.
- Reuse a verified frontend production output only when Git and working-tree state make it safe.
- Make all acceleration paths optional and self-healing through ordinary local builds.

**Non-Goals:**

- Copy, link, or share CMake caches, Ninja files, object files, executables, or `node_modules` between worktrees.
- Download or install `sccache` automatically.
- Change production builds or require compiler caching for any developer.

## Decisions

### Configure sccache through CMake compiler launchers

The launcher finds `sccache` from `PATH` plus conventional platform-specific package-manager installation directories. When executable, it passes `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` only during CMake configuration. A launcher-owned cache-state marker in the build directory records whether caching was successfully configured, allowing the next launch to decide whether it must reconfigure when availability changes.

Alternatives considered:

- Set process-wide compiler environment variables: harder to audit and can affect unrelated commands.
- Use `clcache`: works for MSVC but needs a separate implementation for other supported platforms.
- Share the build directory: unsafe because generated build metadata stores absolute worktree paths.

### Treat cache instrumentation as best effort

The launcher invokes `sccache --show-stats --stats-format=json` before and after building when supported, computes a delta for cache hits, misses, and errors, and prints it. Parsing or command failures disable only reporting. Cache configuration failures or cache-enabled build failures trigger a CMake reconfiguration without launchers and one ordinary-build retry; only a failed retry stops the requested launch.

### Copy frontend artifacts, never link them

The launcher considers registered Git worktrees at the same commit. It excludes a worktree if `git status --porcelain -- web` has output. It selects the newest valid `web/dist/index.html`, copies the entire `web/dist` tree into the current worktree, and invokes the existing freshness predicate. Linking would let a build in one worktree mutate another's assets; copying preserves ownership after the seed step.

Alternatives considered:

- Trust commit equality alone: local frontend modifications could make a copied artifact stale.
- Hash all frontend input files: stricter but slower and unnecessary once both trees are clean at the same commit.
- Require `node_modules`: not needed to serve a verified copied output.

## Risks / Trade-offs

- [A cache tool installs in an unrecognized path] → use `PATH` first, document the optional install hint, and safely fall back.
- [Other builds change global sccache statistics] → label figures as per-launch observed deltas rather than exact attribution.
- [A copied artifact becomes stale while copying] → compare frontend input modification times after the copy; local Vite build remains the fallback.
- [Cache configuration fails after a previously successful configuration] → reconfigure without launchers and continue the requested build.

## Migration Plan

1. Add pure helpers for cache discovery/state and frontend seed eligibility, with focused tests.
2. Integrate helpers into first-time and existing-build configuration flows.
3. Run the Web launcher in a clean worktree with and without `sccache`, confirming both fallback and accelerated paths.
4. Roll back by removing launcher cache settings and seed logic; all build directories and frontend output remain usable through the existing local build flow.
