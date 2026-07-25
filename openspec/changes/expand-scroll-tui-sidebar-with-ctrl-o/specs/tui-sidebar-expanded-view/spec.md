## ADDED Requirements

### Requirement: Ctrl+O controls the regular sidebar detail mode

The TUI SHALL use its existing Ctrl+O global reveal state to control both
transcript tool detail and the regular right sidebar detail mode.

#### Scenario: Enable detail mode with a regular sidebar
- **WHEN** the regular right sidebar is visible and the user presses Ctrl+O while detail mode is off
- **THEN** transcript tool rows SHALL use their existing expanded presentation
- **AND** the right sidebar SHALL switch to its expanded continuous presentation
- **AND** the expanded sidebar viewport SHALL start at its first row

#### Scenario: Disable detail mode
- **WHEN** detail mode is on and the user presses Ctrl+O
- **THEN** transcript tool rows SHALL return to their existing compact presentation
- **AND** the right sidebar SHALL restore its capped top-and-bottom anchored presentation
- **AND** the sidebar scroll position SHALL reset to its first row

#### Scenario: Regular sidebar is unavailable
- **WHEN** the terminal is too narrow for the regular sidebar or ConHost compatibility layout is active
- **AND** the user presses Ctrl+O
- **THEN** transcript detail mode SHALL still toggle
- **AND** no sidebar viewport or sidebar scroll interaction SHALL be created

### Requirement: Expanded sidebar renders all folded sections as one flow

The expanded regular sidebar SHALL render its sections in one continuous
top-to-bottom document and SHALL remove count-based folding from sidebar
sections.

#### Scenario: Capped sections contain hidden rows
- **WHEN** detail mode is on and MCP, changed-file, or TodoWrite data exceeds its compact limit
- **THEN** every item in each section SHALL be rendered
- **AND** no `+N more` row SHALL be rendered for those sections

#### Scenario: Expanded section order
- **WHEN** the expanded sidebar is rendered
- **THEN** its available sections SHALL appear in session-title, MCP, LSP, Files Changed, TodoWrite, Background Tasks, and footer order
- **AND** no flexible spacer SHALL pin later sections to the bottom

#### Scenario: Compact row formatting is retained
- **WHEN** items are rendered in the expanded sidebar
- **THEN** file paths SHALL retain their compact width handling
- **AND** TodoWrite items SHALL retain status-priority ordering
- **AND** each TodoWrite item SHALL use at most two content lines

### Requirement: Expanded sidebar scrolls independently

The expanded regular sidebar SHALL own an independent vertical scroll position
and SHALL not reuse or mutate the transcript scroll position.

#### Scenario: Wheel over overflowing sidebar
- **WHEN** expanded sidebar content exceeds its viewport and the user turns the mouse wheel over the sidebar
- **THEN** the sidebar SHALL scroll by rows in the requested direction
- **AND** the transcript viewport SHALL not move

#### Scenario: Drag the sidebar scrollbar
- **WHEN** expanded sidebar content exceeds its viewport and the user presses and drags its scrollbar
- **THEN** the sidebar scroll position SHALL follow the pointer within the valid scroll range
- **AND** releasing the pointer SHALL end sidebar scrollbar dragging even if release occurs outside the track

#### Scenario: Wheel outside the sidebar
- **WHEN** detail mode is on and the user turns the mouse wheel over the transcript
- **THEN** the existing transcript scrolling behavior SHALL remain unchanged

#### Scenario: Content fits or shrinks
- **WHEN** expanded sidebar content fits in its viewport or shrinks while scrolled
- **THEN** the sidebar scroll position SHALL be clamped to the valid range
- **AND** the scrollbar SHALL not display an overflow thumb when no scrolling is possible
