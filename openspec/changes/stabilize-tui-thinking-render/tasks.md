## 1. Testable pacing primitives

- [x] 1.1 Add adaptive thinking/streaming cadence selection and recent-keyboard activity helpers with named constants.
- [x] 1.2 Add a generation-safe `TuiRedrawPacer` that coalesces periodic requests and records completed loop-frame latency.
- [x] 1.3 Add one-pass, render-window-bounded tool row metadata derivation while preserving full-history compatibility helpers.

## 2. TUI integration

- [x] 2.1 Register the new pacing source in `acecode_testable` and wire periodic thinking/streaming requests through the pacer.
- [x] 2.2 Record keyboard activity and completed frame tickets while leaving immediate correctness-bearing events unthrottled.
- [x] 2.3 Replace per-frame full-history tool metadata scans with the bounded window result.

## 3. Regression coverage and verification

- [x] 3.1 Add deterministic unit and stress-style tests for cadence selection, backpressure, and stale-frame generation races.
- [x] 3.2 Add boundary, FIFO-equivalence, and long-history bounded-window tests for tool metadata.
- [x] 3.3 Build the testable targets, run focused and full unit tests, run the repository quality check, and review the final diff.
