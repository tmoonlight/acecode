---
name: verify-package
description: Build, stage, and locally verify ACECode packages with zero release side effects. Use when asked to verify packaging locally, do a pre-release dry run or local packaging check, stage a CI-equivalent package on macOS or Windows, confirm bundled models.dev/seed resources resolve, or smoke-test TUI and desktop builds. Not for publishing, tagging, npm, update servers, or installers.
platforms: [macos, windows]
compatibility: ACECode skill system
metadata:
  tags: [packaging, verification, local]
---

# Verify Package (Local)

## Purpose

Prove locally that ACECode packaging is correct — file layout, bundled
resources, resource resolution, and runtime startup — without any release
side effect. One command reproduces the CI `package.yml` staging layout and
runs the same resource validations, then smoke-runs the staged binaries.

This skill is the side-effect-free counterpart of `acecode-release`:
verify-package never commits, tags, pushes, publishes, signs, or touches an
update server. It writes only inside the build/staging directories and the
system temp dir.

## Required Inputs

None. The script detects the repo root from its own location, defaults the
build directory to `<repo>/build`, and configures it if missing (MinSizeRel,
`BUILD_TESTING=OFF`, `ACECODE_BUILD_DESKTOP=ON`, Ninja when available, vcpkg
toolchain when `VCPKG_ROOT` is set).

Prerequisite: `web/dist` must exist. If it is missing the script fails with
the exact rebuild command (`cd web && pnpm install --frozen-lockfile &&
pnpm build`) instead of silently verifying the embedded placeholder page.

## Quick Start

macOS (TUI + desktop):

```bash
python3 .acecode/skills/verify-package/scripts/verify_package.py
```

Windows (TUI + desktop):

```powershell
python .acecode\skills\verify-package\scripts\verify_package.py
```

Useful variants:

```bash
# Re-verify an existing build without recompiling
python3 .../verify_package.py --skip-build

# Only one artifact set
python3 .../verify_package.py --target tui
python3 .../verify_package.py --target desktop

# Existing non-default build tree (e.g. build/windows-x64-release)
python3 .../verify_package.py --build-dir build/windows-x64-release

# Where the staged package lands (default: <build-dir>/verify-package-staging)
python3 .../verify_package.py --staging-dir /tmp/ace-verify
```

## What The Script Does

1. Preflight: `web/dist/index.html` exists; cmake is on PATH; the build dir
   is configured when `--skip-build` is used.
2. Configure + incremental build of `acecode` (and `acecode-desktop` for the
   desktop target). Skipped under `--skip-build`.
3. Staging, mirroring the CI Package step: binaries (or the macOS
   `ACECode.app` bundle) + READMEs, then
   `cmake --install --component models_dev_registry` and
   `--component default_seed_bundle` into the staging prefix. The staging
   directory is wiped and rebuilt on every run.
4. Structural checks: staged `share/acecode/models_dev` holds exactly
   `api.json`, `MANIFEST.json`, `LICENSE`, hash-equal to `assets/models_dev`;
   the seed bundle is verified by reusing `scripts/verify_seed_bundle.py`.
   For the desktop target it additionally checks daemon adjacency on
   Windows (`acecode.exe` beside `acecode-desktop.exe`) and the macOS app
   bundle layout (`Contents/MacOS/ACECode`, `Contents/MacOS/acecode-daemon`,
   `Contents/Resources/share/acecode/...`).
5. Runtime probes with an isolated user profile (temp `HOME`/`USERPROFILE`,
   so your real `~/.acecode` is never touched):
   - TUI: run the staged binary with `--version`, then
     `--validate-models-registry`; the latter must report `registry OK` with
     a source inside the staged `share/acecode/models_dev` tree, proving
     resource resolution does not fall back.
   - Desktop: launch the staged app, wait for it to stay alive, then
     terminate it. This proves startup, not visual correctness — eyeball the
     UI yourself if the change is UI-facing.
6. Report: one `[PASS]`/`[FAIL]` line per check, then
   `verify-package: PASS` (exit 0) or `verify-package: FAIL` (exit 1).

## Reading The Report

- A failed `tui models registry resolution` with a non-`models_dev` source
  means the packaged resource layout or the search-path logic broke — this
  is the class of bug that a bare build-dir run can never catch.
- Known expected behavior (not a bug): running the bare `build/acecode`
  binary without a staged `share/` tree logs a registry fallback warning.
  The staged run in this skill exists precisely to avoid that confusion.
- `desktop launch` only proves the process starts and stays alive. Visual
  and interaction verification stays manual.
- `desktop launch` reports `[SKIP]` when an ACECode desktop instance is
  already running: the single-instance guard would make a second instance
  exit immediately, so the probe would say nothing. Close ACECode and
  re-run to exercise startup.

## Guardrails

- Never add publishing behavior here: no git operations, no npm, no
  `aceupdate.json`, no update-server uploads, no signing or notarization.
  Release work belongs to the `acecode-release` skill.
- Do not bypass the `web/dist` preflight — verifying the embedded fallback
  page proves nothing about the web UI.
- Linux is out of scope; the script mechanically supports a flat Linux
  layout for tests, but real Linux verification happens in CI.
- The desktop launch probe runs a real GUI process. Run it on a machine
  with a desktop session; in headless environments use `--target tui`.
