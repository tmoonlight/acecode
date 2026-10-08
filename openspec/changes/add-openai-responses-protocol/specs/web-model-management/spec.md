## ADDED Requirements

### Requirement: OpenAI protocol editor
The existing model editor SHALL expose Chat Completions and Responses protocol choices for OpenAI profiles. Selection SHALL persist through create, edit and test actions. Existing profiles SHALL display Chat Completions by default, and non-OpenAI profiles SHALL not send this field.

#### Scenario: Create and test a Responses model
- **WHEN** a user selects Responses for an OpenAI profile and tests or saves it
- **THEN** the daemon receives api_protocol responses and uses that protocol

#### Scenario: Reopen or change provider
- **WHEN** a saved Responses profile is edited
- **THEN** the editor restores Responses, and switching to another provider removes the OpenAI-specific option
