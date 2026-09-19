---
name: development-environment
description: Start ACECode's Web, Desktop, or TUI development environment safely, reusing a compatible build from a registered Git worktree when possible.
---

# Development Environment

Use this skill when the user asks to run, start, or open the ACECode development environment.

## Select the target

If the request does not name a target, ask exactly which development surface to run:

- **Web** — starts the daemon-backed browser UI.
- **Desktop** — starts the native desktop shell against the local development frontend.
- **TUI** — starts the terminal interface in a new terminal window.

Do not start anything until the user selects one target. If they name a target, proceed without repeating the question.

## Use the shared launcher

Run the target-specific repository launcher instead of implementing launch logic in the conversation:

```powershell
.\scripts\dev_web.bat
.\scripts\dev_desktop.bat
.\scripts\dev_tui.bat
```

On macOS or Linux:

```bash
./scripts/dev_web.sh
./scripts/dev_desktop.sh
./scripts/dev_tui.sh
```

Pass `--build-dir <path>` only when the user explicitly supplies a candidate build directory. Do not copy `acecode`, `acecode-desktop`, DLLs, or other build artifacts between worktrees.

## Build reuse and rebuild policy

The launcher discovers worktrees with `git worktree list --porcelain`, reads candidate `CMakeCache.txt` files, and validates the configured source path, current commit, platform, architecture clues, required executable, and Desktop configuration before reuse.

Every launch incrementally builds the verified target, so source changes are incorporated even when the configured build is reused. Web and Desktop also refresh frontend assets when their inputs are newer than `web/dist`.

If no compatible configured build exists, the launcher reports the platform CMake preset and asks for confirmation before configuration. Preserve that safety boundary:

- Windows target-specific batch launchers automatically approve this first configuration so they work when double-clicked.
- For the shared Python launcher and POSIX target-specific launchers, state that configuration is needed, name the preset, and ask the user for explicit confirmation before adding `--yes`.
- If the user declines, do not configure, compile, or start a surface.

The shared launcher calls the existing Python surface launchers: `scripts/dev_web.py` for Web and `scripts/dev_desktop.py` for Desktop. Web uses a worktree-isolated runtime directory and opens its resulting local URL; Desktop opens its application window; TUI opens a new terminal window.

## Report outcome

After a successful command, report the selected target and whether the build was reused or compiled. For Web, include the URL printed by the launcher. If startup fails, provide the launcher error and do not claim the environment is running.
