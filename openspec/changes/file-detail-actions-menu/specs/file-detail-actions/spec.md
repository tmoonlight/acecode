## ADDED Requirements

### Requirement: File detail commands have a shared menu
File details SHALL group file-level icon actions in a single trailing breadcrumb menu, including source copy, wrap, Markdown mode, opening HTML in the built-in browser, Office zoom, and slide navigation when applicable.

#### Scenario: Path overflows or has no workspace breadcrumbs
- **WHEN** the path is long or outside the workspace
- **THEN** applicable actions remain accessible at the right edge independently of path scrolling

#### Scenario: Preview changes
- **WHEN** the user changes tabs, edits text, or reaches a zoom or slide limit
- **THEN** the menu reflects the current file, content and available operations

### Requirement: Both file detail menus can open the containing folder
Desktop Shell and compatible WebApp file details SHALL offer 打开文件夹 through both the trailing menu and the document context menu, including during editing.

#### Scenario: Open the current file location
- **WHEN** the user chooses 打开文件夹 from either menu
- **THEN** the system file manager is requested for the same resolved file in its containing folder
