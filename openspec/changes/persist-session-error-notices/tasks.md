## 1. Durable errors

- [x] 1.1 Persist error notices at the shared transcript dispatch boundary with UUID identity and transcript-only metadata; verify native tests cover disk reload, repeat failures, event identity, and exclusion from current/resumed/compacted model context.
- [x] 1.2 Reconcile persisted errors by ID in Web history and replay while preserving legacy occurrence semantics; verify reducer regression tests for switching, reload, overlapping replay, and repeated identical failures.

## 2. Delivery checks

- [x] 2.1 Document the additive daemon message contract and verify strict OpenSpec validation plus diff checks.
- [x] 2.2 Run relevant Windows native regression tests, full Web tests, Web build, and a browser check of restored red error cards; record results and runtime limitations.
- [x] 2.3 Preserve the original storage failure when persisting its error notice also fails, update blocked-input/failed-compaction expectations for transcript-only errors, and pass focused plus full Windows release regressions.
- [ ] 2.4 Keep the checked-append facade in the existing history implementation to satisfy the file-size limit; verify all strict gates individually and rerun native session regressions after the move.
