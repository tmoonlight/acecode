## 1. Implementation

- [x] 1.1 Add measured console font selection and regression coverage for proportional substitutions, missing fonts, bold width mismatch, fractional/invalid metrics and unavailable measurement; verify with the focused Node test.
- [x] 1.2 Integrate selection into ConsoleDock before xterm creation using the owner document and the existing font size; register the test and verify the component diff preserves PTY lifecycle and layout.

## 2. Verification

- [x] 2.1 Reproduce the old sparse text and verify the new resolver with real xterm in the original Linux WebKit environment, including normal/bold ASCII and Chinese output; record metrics and a screenshot.
- [x] 2.2 Run the full web tests, production web build, strict OpenSpec validation and git diff --check; record outcomes and confirm unrelated workspace changes are preserved.

## 3. Local WSL package delivery

- [x] 3.1 Build Linux x64 CLI and Desktop from the current checkout with the verified frontend embedded, stage the CI resource layout, and verify package checksums, executable versions and resource resolution.
- [x] 3.2 Install the local package beside the retained official release, switch the WSL launchers, and verify the actual Desktop console font rendering without any Linux font workaround; record the installed hashes and rollback path.
