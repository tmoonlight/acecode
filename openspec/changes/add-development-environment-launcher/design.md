# Design

## Context

See `proposal.md` for motivation and `specs/development-environment-launcher/spec.md` for behavioral requirements. The repository already has Python-backed Web and Desktop launchers with thin platform wrappers. Web currently accepts an explicit build directory and runtime directory, while Desktop accepts an explicit build directory and rebuilds `web/dist` when its inputs are newer. TUI has no comparable wrapper.

## Goals / Non-Goals

**Goals:**

- Provide one shared Python orchestration entry point and thin Windows/POSIX wrappers.
- Reuse existing Web and Desktop launch scripts rather than duplicating their surface-specific behavior.
- Reliably identify compatible builds from Git-registered worktrees before proposing a local build.
- Keep development daemons isolated per worktree and make start outcomes visible.

**Non-Goals:**

- Change production daemon, Desktop, TUI, or CMake behavior.
- Copy build artifacts, DLLs, or resources between worktrees.
- Search arbitrary user directories outside worktrees registered by the current Git repository.
- Automatically compile without interactive confirmation.

## Decisions

### Use a Python orchestration layer with thin platform wrappers

A new `scripts/dev_environment.py` will parse the selected target and coordinate discovery, confirmation, incremental building, and launch. Dedicated target wrappers will select Python and a fixed target: `dev_web.bat` / `dev_web.sh`, `dev_desktop.bat` / `dev_desktop.sh`, and `dev_tui.bat` / `dev_tui.sh`. Python matches the existing launcher implementation and is portable across Windows, macOS, and Linux.

Alternatives considered:

- Separate shell implementations would duplicate platform logic and Git/CMake parsing.
- Extending `dev_web.py` or `dev_desktop.py` would couple target-independent discovery to a single surface.

### Discover builds only from Git worktree registrations

The orchestration layer will inspect only the current worktree's candidate build directories and read `CMakeCache.txt` to confirm that each build was configured from the current source directory. It validates platform, generator architecture clues, selected target configuration, and executable presence. Registered peer worktrees are used only for safe compiler-cache and frontend-artifact acceleration; their CMake/Ninja directories are never built or launched for the current worktree.

Alternatives considered:

- Scan sibling or home directories: this can find unrelated repositories and is slower.
- Compare only executable timestamps: timestamps do not prove the source revision or Desktop capability.

### Delegate surface-specific startup

The shared launcher performs the target's incremental CMake build after it validates or configures a build directory. For Web and Desktop, it then invokes the existing Python surface launchers directly, forwarding the validated `--build-dir`; this avoids recursing through target wrappers. The Desktop surface launcher continues to refresh frontend assets, and the shared launcher adds the equivalent freshness check before Web launch. Web receives a deterministic runtime directory below the current worktree's ignored development state. TUI is started from the validated `acecode` binary in a new terminal window using platform-specific process invocation.

Alternatives considered:

- Reimplement Web and Desktop startup in the new tool: would create two sources of truth for Web assets and Desktop development-mode behavior.
- Run TUI in the current terminal: conflicts with the agreed ability to continue launcher work after opening TUI.

### Auto-configure missing builds from Windows direct entry points

Double-clicked batch files do not provide a reliable input stream for the shared launcher's confirmation prompt. Each Windows target entry point will therefore append `--yes` when it calls `dev_environment.py`, approving only the missing-build configuration path. The shared Python launcher remains conservative for callers that invoke it directly, and POSIX wrappers retain their interactive confirmation behavior.

### Initialize the Windows C++ toolchain in batch entry points

The three Windows target wrappers will call a shared batch helper before invoking Python. The helper locates `vswhere.exe` from the Visual Studio installer location, asks it for an installation containing the x64 C++ tools component, and calls that installation's `VsDevCmd.bat` with `-arch=amd64 -host_arch=amd64`. It preserves the caller's command context while supplying the standard-library and linker paths that CMake needs. If discovery fails, it reports the Build Tools C++ workload requirement and exits before Python runs.

Alternatives considered:

- Require callers to use a Developer Command Prompt: contradicts direct double-click and normal-shell entry point behavior.
- Duplicate Visual Studio path discovery in all three wrappers: risks inconsistent architecture and error handling.

### Require interactive confirmation only for a new configuration

When discovery cannot produce a compatible result, the tool calculates the native CMake preset for the selected target and prints configure/build commands. It asks a yes/no question only when stdin is interactive; non-interactive invocations fail with the same instructions rather than implicitly configuring. Once a build directory has been validated or configured, the target's incremental build runs without another confirmation so each launch reflects current sources. First-time development configurations pass `-DBUILD_TESTING=OFF`, because unit-test dependencies are optional in the vcpkg manifest and are not needed to run a development surface.

Alternatives considered:

- Auto-build: potentially expensive and unexpected.
- Always fail: forces developers to reconstruct platform-specific presets manually.

## Risks / Trade-offs

- [A valid external build is configured with an unusual directory layout] → inspect standard build directories plus explicit `--build-dir`; require an exact `CMakeCache.txt` source match.
- [CMake does not expose a fully portable architecture field] → require a runnable platform-native executable and compare configured generator/platform fields when present; reject uncertain configurations.
- [A worktree-specific runtime path is untracked] → create it under the repository's ignored `.acecode` development state and document it in the launcher output.
- [A platform lacks a supported graphical terminal launcher] → report the exact TUI executable command instead of silently starting TUI in the caller's terminal.

## Migration Plan

1. Add the shared launcher and thin wrappers without changing existing Web or Desktop launch commands.
2. Add the repository-local skill, directing requests through the shared policy.
3. Validate no-target selection, verified reuse, rejected reuse, declined rebuild, and each platform wrapper through focused tests or script help checks.
4. Roll back by removing the new launcher, wrappers, and skill; existing Web and Desktop launchers remain unchanged.
