## ADDED Requirements

### Requirement: Invalid Git ERE diagnostics are locale-independent
ACECode SHALL classify an invalid extended regular expression as an invalid-pattern
tool error without requiring Git's stderr prose to be English.

#### Scenario: Git reports an invalid pattern in a translated locale
- **WHEN** `git grep -E` fails and its localized diagnostic identifies the submitted
  `-e` pattern
- **THEN** the grep tool returns the stable `Invalid Git extended regex` error prefix

#### Scenario: Git fails for another reason
- **WHEN** `git grep` fails without identifying the submitted pattern as the `-e`
  argument
- **THEN** the grep tool preserves the generic Git failure classification
