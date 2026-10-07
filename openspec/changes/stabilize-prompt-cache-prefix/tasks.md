## 1. Stable context and checklist

- [x] 1.1 Implement versioned hidden context snapshots and appended updates; verify multi-turn, hook persistence, changed instructions, plan/security changes and session reload regressions.
- [x] 1.2 Freeze the skill index and remove repeated todo injection; install fresh context and current todos with successful compact checkpoints; verify unchanged prefixes, checklist restoration and final-summary ordering.

## 2. Compaction and child models

- [x] 2.1 Reuse the normal model-facing prefix/tools for supported compaction with validated tool-free fallback; verify native/text tool rejection, overflow/cancellation and automatic/manual request equivalence.
- [x] 2.2 Inherit the current parent model for unspecified ordinary/star subagents, preserve explicit override and fallback, document behavior and verify mesh parity tests.

## 3. Compatible cache routing

- [x] 3.1 Add stable session keys on verified endpoint allowlists, synchronized provider state and narrow unsupported-field downgrade; verify streaming/nonstreaming transport, endpoint matching, rebuild stability and error/cancellation preservation.

## 4. Integrated validation

- [x] 4.1 Run the targeted native regression suites, then Windows full unit tests and applicable static checks; record exact commands/results and fix task-related failures.
- [x] 4.2 Validate OpenSpec strictly, review all task diffs with git diff --check and audit the five requirements against tests, preserving unrelated work.
