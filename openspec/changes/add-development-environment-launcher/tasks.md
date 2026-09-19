# Tasks

## 1. Shared launcher

- [x] 1.1 Add the cross-platform Python launcher with interactive target selection, target parsing, and platform-specific CMake preset selection; verify its help output and target-validation behavior.
- [x] 1.2 Implement Git worktree build discovery and compatibility validation for source revision, executable, platform/architecture, and Desktop configuration; verify focused tests cover accepted and rejected candidates.
- [x] 1.3 Implement automatic incremental builds for verified configured builds, confirmation only for missing/incompatible configuration, and explicit non-interactive or declined-configuration failures; verify no configure command runs without confirmation.

## 2. Target startup and entry points

- [x] 2.1 Delegate Web and Desktop starts to the existing Python surface launchers after refreshing frontend assets, use an isolated Web runtime directory, and report launch outcomes; verify forwarded arguments with stub launchers.
- [x] 2.2 Start TUI from the verified executable in a new terminal window on supported platforms and report unsupported terminal-launch behavior; verify command construction with focused tests.
- [x] 2.3 Provide target-specific `dev_web`, `dev_desktop`, and `dev_tui` Windows/POSIX entry points that select a fixed target and forward supported arguments unchanged; verify syntax and `--help` delegation on available platforms.

## 3. Windows toolchain and verification

- [x] 3.1 Add a shared Windows Visual Studio developer-environment helper and invoke it from each target-specific batch entry point; verify a normal shell receives C++ compiler include paths.
- [x] 3.2 Make Windows target-specific entry points automatically approve a missing-build configuration, while preserving explicit confirmation for Python and POSIX callers; verify wrapper argument forwarding and focused launcher tests.
- [x] 3.3 Run Python compilation checks and `openspec validate add-development-environment-launcher --strict`.
