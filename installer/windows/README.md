# ACECode Windows installer

Two installers live here:

- `make_setup.py` builds the self-made installer (`acecode-setup.exe` plus an embedded package, no third-party packaging tool). It takes over an existing Inno installation in the same folder. Design, build steps and verification are in [docs/windows-installer.md](../../docs/windows-installer.md).
- `acecode.iss` / `build.ps1` below are the earlier Inno Setup package, kept until the self-made installer replaces it in releases.

## Inno Setup package

Navy-themed Inno Setup package for ACECode Desktop.

## Build

Requires a current `acecode.exe` / `acecode-desktop.exe` Release build, CMake, Python (Pillow), and Inno Setup 6/7.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\windows\build.ps1
```

The compiled setup is written to `installer/windows/output/ACECode-<version>-windows-x64-setup.exe`.

## ACEModel page

The last wizard page asks for an ACEModel API key. If the user fills it in, the installer writes `moonlight`, `starrylight`, and `aurora` with a 250,000-token fallback context window and the default `vision` / `tool_use` capabilities into `%USERPROFILE%\.acecode\config.json`. Server-provided context metadata remains authoritative when the models are probed, and explicit manual capability overrides remain unchanged. The key can be skipped.

## Verify the seeder

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\windows\seed_acemodel.test.ps1
```
