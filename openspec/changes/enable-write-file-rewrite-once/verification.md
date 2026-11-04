# Verification

Date: 2026-11-04

- Native Release unit target built successfully using `scripts/dev_windows_env.bat` followed by `cmake --build build/windows-x64-msvc --config Release --target acecode_unit_tests --parallel 4 -- /p:UseStructuredOutput=false`.
- Focused native run passed **39/39 tests** with filter `ToolRewrites.*:ToolProtocolNames.*:ToolExecutorNameResolution.*:ToolRewritesHandler.*:WebServerHttp.ToolRewritesRoundTripPersistsJsonAndAppliesToProcess`.
- Coverage includes missing/disabled settings, already-enabled custom/empty mappings, completion persistence, unchanged files on later startup, API disable followed by startup, subsequent custom/removal choices, legacy filenames, malformed settings, name conflicts, failed writes and successful retry.
- `openspec validate enable-write-file-rewrite-once --strict` passed.
- `python scripts/layers/check_layers.py --repo . --layout final --strict --output build/write-file-rewrite-layer-lint.json` passed with zero findings.
- `python scripts/layers/check_ownership.py --repo . --strict --final --output build/write-file-rewrite-ownership.json` passed.
- `git diff --check` passed.

Build evidence is in `build/write-file-rewrite-build-final.log`; test evidence is in `build/write-file-rewrite-tests.log` and `build/write-file-rewrite-tests.xml`.

The first build hit MSVC structured-output UTF-8 parsing error MSB8084. The retry disabled that diagnostic mode. A namespace qualification in the new HTTP assertion was corrected. Linking then exposed three stale test objects referencing standard-library symbols absent from the selected MSVC 14.39 toolchain; only those generated objects were removed and rebuilt, after which the target linked successfully.

No frontend behavior changed. Existing edits to session permissions, web helpers, ChatView and API code were retained. The pre-existing API documentation and HTTP smoke-test files received only targeted additions for this change. Full native-suite execution, installed-app UI checks, other platforms and release publication were not performed.
