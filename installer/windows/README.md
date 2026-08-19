# ACECode Windows installer

Navy-themed Inno Setup package for ACECode Desktop.

## Build

Requires a current `acecode.exe` / `acecode-desktop.exe` Release build, CMake, Python (Pillow), and Inno Setup 6/7.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\windows\build.ps1
```

The compiled setup is written to `installer/windows/output/ACECode-<version>-windows-x64-setup.exe`.

## ACEModel page

The last wizard page asks for an ACEModel API key. If the user fills it in, the installer writes `moonlight` and `starrylight` into `%USERPROFILE%\.acecode\config.json`. The key can be skipped.

## Verify the seeder

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File installer\windows\seed_acemodel.test.ps1
```
