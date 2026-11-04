# Native Windows installer release

The Windows matrix builds `acecode-setup` and embeds the exact portable ZIP into
`ACECode-<version>-windows-{x64,arm64}-setup.exe`. Both setup files are required by
`scripts/verify_release_assets.py` and are included in `SHA256SUMS.txt`. The ZIP
self-update packages remain unchanged.

Each setup is inspected on Windows as a data file: executable architecture,
embedded uninstaller, metadata, payload size, and hashes must match its release.
The x64 job also installs and uninstalls in an isolated profile. ARM64 receives
PE/resource/payload checks; this does not constitute an ARM64 hardware UI test.
Windows signing is not configured by this workflow.

## Local verification

```powershell
python scripts/verify_windows_setup.py `
  --setup ACECode-1.0.21-windows-x64-setup.exe `
  --payload-zip acecode-windows-x64.zip `
  --version 1.0.21 --target windows-x64 --smoke
```

The smoke test uses a temporary `USERPROFILE` and `ACECODE_SETUP_TEST_ROOT`. It
checks the installed runtime version and installation records, then uninstalls
and verifies that a user-data sentinel survived. It refuses to run while another
installer sandbox has a registered installation. Run it after other installer
acceptance tasks finish.

Pass `--install-volume L:/` to install on a different volume while retaining a
separate TEMP sandbox. The Windows x64 CI job uses the runner's profile drive for
this check. Locked launcher files are retired beside the installation directory
so uninstallation does not depend on cross-volume rename support; subsequent
installer runs for that location clean up those retired copies.

## Update download server

After the standard six-platform ZIP and macOS PKG mirror succeeds, download the
two setup files, their exact ZIPs and release checksums into a dedicated directory:

```powershell
gh release download v1.0.21 --repo tmoonlight/acecode `
  --dir build/release-verification-1.0.21/windows-installers `
  --pattern '*windows*-setup.exe' --pattern 'acecode-windows-*.zip' `
  --pattern SHA256SUMS.txt
python scripts/mirror_windows_installers.py --version 1.0.21 `
  --assets build/release-verification-1.0.21/windows-installers `
  --report build/release-verification-1.0.21/windows-installers.json
```

The helper checks both architectures before writing any downloads. It atomically
copies versioned files and aliases `ACECode-windows-{x64,arm64}-setup.exe`, then
downloads all four public URLs and checks their MIME type, byte size and SHA256.
The server must serve `.exe` as `application/octet-stream`. The helper never adds
EXE files to `aceupdate.json`; it verifies that the manifest did not change during
this step. If download verification fails, repair the server and rerun for the
same version without moving the published tag.
