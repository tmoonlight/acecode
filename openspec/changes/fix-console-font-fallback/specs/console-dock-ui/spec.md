## Purpose

Provide a readable embedded console whose fixed character grid stays consistent across browser and desktop platforms even when the user's preferred terminal fonts are unavailable.

## ADDED Requirements

### Requirement: Monospaced console font fallback

The console SHALL retain uniform character advances for ordinary ASCII text in both normal and bold rendering when preferred terminal fonts are missing or substituted with a proportional face. The application SHALL select a usable monospace fallback without requiring the user to install fonts or modify operating-system font configuration.

#### Scenario: Missing first font is substituted proportionally
- **WHEN** a missing preferred font resolves to a proportional substitute even though later fallback fonts exist
- **THEN** the console rejects that substitute and uses a family that renders ASCII text with equal advances
- **AND** the invalid family does not remain ahead of the selected family in the effective font choice

#### Scenario: Every named preference is unavailable
- **WHEN** no named terminal font candidate renders with valid uniform advances
- **THEN** the console uses the browser's generic monospace font
- **AND** terminal startup and typing remain available without changing system fonts

#### Scenario: Bold text would use inconsistent widths
- **WHEN** a candidate renders uniform normal text but inconsistent bold text or changes the cell advance between weights
- **THEN** the console skips that candidate so normal output and bold prompts stay aligned

#### Scenario: Font measurement is unavailable
- **WHEN** the environment cannot provide usable font measurements
- **THEN** the console starts with the browser's generic monospace font instead of failing to open

#### Scenario: New tabs and mixed-language output
- **WHEN** a user creates a terminal tab and prints normal text, bold prompts, Chinese text and ASCII punctuation
- **THEN** the tab uses the selected monospace family at its configured size
- **AND** wide characters, wrapping, resizing and terminal input continue to follow the console's existing character-grid behavior
