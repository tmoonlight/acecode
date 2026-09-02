## Why

The Linux unit-test job currently fails for three independent reasons: an invalid
config mutation target enters fatal startup recovery, detached model-context probe
threads race process teardown, and the Linux update ZIP fixture omits the newly
required default seed bundle. These failures hide regressions on every pull request
and the detached-thread race can also affect normal process shutdown.

## What Changes

- Reject an existing non-regular config mutation target as a persistence error before
  invoking startup-oriented config recovery.
- Replace detached model-context probe threads with one lifecycle-owned background
  worker that can cancel an active HTTP request, discard queued probes, and join
  before process-global dependencies are destroyed.
- Make the test cache reset wait for probe quiescence so tests cannot leak remote work
  into later cases.
- Bring the Linux update ZIP contract fixture in sync with the required default seed
  bundle.
- Recognize Git's invalid-ERE diagnostics without depending on the process language.
- Isolate the stale remote-control selection regression from unrelated keepalive
  retries and wait for its control queue deterministically.
- Add focused regression coverage and rerun the complete Linux unit-test suite.

## Capabilities

### New Capabilities

- `runtime-background-probes`: lifecycle and shutdown guarantees for nonblocking
  model-context endpoint probes.
- `config-mutation-safety`: failure semantics for invalid config persistence targets.
- `grep-error-diagnostics`: locale-independent classification of invalid Git extended
  regular expressions.

### Modified Capabilities

None.

## Impact

- Runtime code: `src/provider/model_context_resolver.*` and
  `src/config/config_mutation.cpp`.
- Tests and release contracts: provider/config unit tests, grep-tool tests, and
  `tests/scripts/linux_update_zip_test.sh`.
- No config schema, daemon API, UI, or packaged file-layout change.
