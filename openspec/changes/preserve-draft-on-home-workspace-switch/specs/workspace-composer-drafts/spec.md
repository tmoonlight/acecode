## ADDED Requirements

### Requirement: Restore independent workspace drafts
The home composer SHALL restore the selected workspace's draft when the user changes the workspace through either the dropdown or sidebar. A workspace without a folder SHALL have an independent draft. A delayed response for an earlier selection SHALL NOT replace or persist over the current selection's draft.

#### Scenario: Select a workspace with a saved draft
- **WHEN** the user selects a workspace whose persisted draft contains text
- **THEN** the editor displays that text after loading, including when the editor initially renders empty
- **AND** the loading transition does not persist an empty draft over the saved content

#### Scenario: Switch workspaces before draft loading finishes
- **WHEN** a draft response arrives after another workspace has been selected
- **THEN** the response does not replace the selected workspace's editor content

#### Scenario: Send a workspace draft
- **WHEN** sending succeeds
- **THEN** only the submitted workspace's draft is cleared
- **AND** a failed send preserves that draft
