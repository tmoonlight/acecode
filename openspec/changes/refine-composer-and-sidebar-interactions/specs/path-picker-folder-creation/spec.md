## Purpose

Allow users to create and select a folder within the current directory without leaving the path picker or losing recoverable input errors.

## ADDED Requirements

### Requirement: Folder creation uses a localized dialog
The folder picker SHALL open a separate localized creation dialog with an initially blank name, confirm and cancel actions, and a close button. Clicking the backdrop SHALL NOT close it. Cancelling SHALL leave the parent picker open and create nothing.

#### Scenario: Cancel a new folder
- **WHEN** the user cancels, closes, or dismisses the creation dialog with Escape while no request is in progress
- **THEN** the folder picker remains available and no folder is created

### Requirement: Folder names are validated before creation
The dialog SHALL reject blank names, names over 50 UTF-16 code units, and names invalid for the target filesystem. Creation errors SHALL retain the dialog and input while showing an accessible error. Confirmation SHALL be unavailable for blank input or during an active request.

#### Scenario: Duplicate or invalid name
- **WHEN** validation rejects the name or the server rejects creation
- **THEN** the dialog remains open and the user can correct the name and retry

### Requirement: Successful creation selects the folder
After creating a folder, the picker SHALL close the creation dialog, refresh the parent directory and select the new folder for the existing main selection action.

#### Scenario: Create and select
- **WHEN** a valid folder is created in the current directory
- **THEN** the refreshed picker selects it and the user can confirm it as the chosen directory
