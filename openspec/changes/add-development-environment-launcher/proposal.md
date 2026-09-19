# Proposal

## Why

Developers currently need to choose and invoke separate Web or Desktop launchers manually, while TUI startup and reuse of compatible build outputs require repository-specific knowledge. A single cross-platform entry point should guide the target selection and safely reuse an existing build from a related worktree when possible.

## What Changes

- Add a cross-platform development-environment launcher that starts the Web daemon, Desktop shell, or terminal UI using existing repository launchers and build outputs.
- Add dedicated Windows and POSIX entry points for Web, Desktop, and TUI so developers can start the desired target without providing a target argument or using an agent skill.
- Validate reusable builds from the current worktree against its source directory, platform, architecture, and requested target; use related worktrees only for safe cache and frontend-artifact acceleration.
- Incrementally rebuild every verified build before launch so source changes are incorporated; require confirmation only before configuring a missing or incompatible build.
- Initialize the Windows Visual Studio C++ developer environment from the direct launchers so incremental builds work from a normal shell or double-clicked batch file.
- Allow Windows direct launchers to configure a missing target build automatically, so double-clicked entry points do not wait for unavailable confirmation input.
- Give each Web development workspace an isolated daemon runtime directory and open the selected development surface after a successful start.
- Add a repository-local skill that asks for a target when omitted and applies the same validation and launch policy.

## Capabilities

### New Capabilities

- `development-environment-launcher`: Guided, cross-platform selection, validation, and startup of ACECode Web, Desktop, and TUI development environments.

### Modified Capabilities

- None.

## Impact

- New shared launcher logic and thin `.bat` and `.sh` entry points under `scripts/`.
- Existing `scripts/dev_web.*` and `scripts/dev_desktop.*` remain the surface-specific launch mechanisms invoked by the new launcher.
- New repository-local skill under `.agents/skills/`.
- CMake preset selection and Git worktree metadata are used for build discovery and validation; no application protocol or production runtime behavior changes.
