# Verification

## Integrated source checks (2026-11-05)

- Installer implementation finished before integration. Its full native run reported 6,922 tests: 6,909 passed, 13 skipped, zero failed, with one additional disabled test.
- After the cross-volume uninstall fix, the final Windows CLI, Desktop, setup and unit-test targets rebuilt successfully. Focused installer, upgrade, seed, rewrite, permission, PA recovery and compaction tests: 362 run, 361 passed, one screenshot-only test skipped.
- `pnpm test`: 3,536 custom checks passed; the Node test section also passed, zero failures. `pnpm build` and generated regex compatibility checks passed.
- Strict layer, document-path, include normalization (src/tests), file-size, ownership and migration-map checks passed. Refactor tooling self-tests passed.
- Python packager, setup verification, installer mirror and complete-release inventory tests passed. Both workflow YAML files parsed successfully.
- Strict OpenSpec validation passed for this change and `enable-write-file-rewrite-once`.

## Installer package checks

- Read actual setup resources and confirmed exact ZIP bytes, version, architecture, payload/uninstaller hashes and sizes.
- Isolated silent install, runtime version, file and registry installation records, uninstall removal and preservation of a user-data sentinel passed.
- Reproduced the original cross-volume defect: installation on L: with TEMP on N: returned install exit 0, uninstall exit 1, leaving the installation directory.
- After same-volume locked-file retirement, the same cross-volume lifecycle check passed. CI now exercises installation on the Windows runner's profile drive while TEMP remains separately isolated.
- The installer owner's graphical acceptance covered welcome/install/completion/uninstall animation, model-key merge, running-process reinstall rejection, and low-height/high-DPI window fitting.

## Limits

Windows installers remain unsigned. ARM64 native execution and UI are not locally tested; CI validates its PE architecture and embedded payload identity. Platform-specific skips and the disabled native test are not counted as passes. Tagged artifacts and public downloads are verified by the release workflow and retained in the release report.

## Versioned release candidate

The final 1.0.21 Windows candidate ZIP has SHA256 `92f516bc8286f80d2f0fc8016acb78b1ab6f870aa0388e9801ce5029c7d3956f` (26,312,980 bytes). Its executable reports `acecode v1.0.21`.

Using the actual v1.0.20 ZIP to initialize an isolated profile, upgrading this candidate advanced seed `2026-10-16.1` to `2026-11-05.1`, installed and recorded ownership of `inline-visuals`, repaired an absent managed hook with an equal marker, preserved a user-modified skill, and did not downgrade a newer marker. The 1.0.21 setup passed resource verification and the cross-volume install/uninstall smoke. These package checks are repeated against the downloaded tagged Windows asset before release completion; final GitHub asset hashes are recorded in the release report.
