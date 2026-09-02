## 1. Runtime and Persistence Fixes

- [x] 1.1 Replace detached model-context warmups with a cancellable, joinable
  single-worker queue while preserving key deduplication and cache warming.
- [x] 1.2 Make the model-context test reset cancel pending work and wait for worker
  quiescence before clearing state.
- [x] 1.3 Reject filesystem errors and existing non-regular config mutation targets as
  persistence failures before loading config content.
- [x] 1.4 Make invalid Git ERE classification independent of English stderr wording.

## 2. Contract Fixtures and Regression Coverage

- [x] 2.1 Copy the canonical default seed tree into the Linux update ZIP contract
  fixture.
- [x] 2.2 Add focused model-context coverage for immediate fallback followed by cached
  endpoint metadata and retain cross-platform config failure coverage.
- [x] 2.3 Verify the grep invalid-pattern contract under the current non-English Git
  locale.
- [x] 2.4 Fence the stale remote-control selection test through the control queue and
  remove unintended keepalive retries from its activation count.

## 3. Verification

- [x] 3.1 Build `acecode_unit_tests` and run focused config, model-context, session
  binding, and Linux update ZIP tests.
- [x] 3.2 Run the complete Linux CTest suite, strict OpenSpec validation, code-quality
  checks, and `git diff --check`.
