## ADDED Requirements

### Requirement: Xcode discovery without xcode-select
The system SHALL locate usable Xcode installations from the configured path, DEVELOPER_DIR, xcode-select, /Applications, ~/Applications and Spotlight, and SHALL run simulator tooling from the chosen Xcode's own Developer directory. A configured path SHALL take precedence; an unusable configured path SHALL be reported with its reason.

#### Scenario: Green-installed Xcode
- **WHEN** xcode-select points at the Command Line Tools and Xcode lives in ~/Downloads/Xcode.app
- **THEN** discovery finds it (Spotlight) or the user selects it in Settings, and all simulator actions work without changing xcode-select

### Requirement: Simulator control helper
The system SHALL drive simulators through an owned helper process that exits with its parent, uses bounded framing, and reports missing CoreSimulator, SimulatorKit or HID support as actionable errors. Input SHALL use the HID transport supported by the loaded CoreSimulator version.

#### Scenario: Landscape app
- **WHEN** the simulator shows a landscape interface
- **THEN** screenshots are delivered upright at one pixel per point and touches at screenshot coordinates hit the same element

### Requirement: Model tools with per-device consent
The system SHALL expose `ios_simulator` (auto-approved, serial) and `ios_simulator_build` (ordinary permission). Before the first control of a simulator the user SHALL be asked once; refusals, cancellations and timeouts SHALL deny; grants SHALL be remembered and revocable. State-changing actions SHALL be refused in plan mode.

#### Scenario: First use of a device
- **WHEN** the agent attaches to a simulator that has never been allowed
- **THEN** the user is asked "允许 ACECode 控制模拟器…" and no input reaches the device unless they allow it

### Requirement: Live interactive panel
The system SHALL show a live view of the attached simulator in the side panel, opened automatically on attach, and SHALL forward the user's pointer, wheel, keyboard and paste input to the simulator.

#### Scenario: User taps in the panel
- **WHEN** the user clicks the panel image
- **THEN** the click is delivered as a touch at the corresponding device point

### Requirement: macOS-only distribution
The helper SHALL be built and packaged only for macOS (app bundle, portable package, installer, update archive, npm darwin packages). Other platforms SHALL neither build nor ship it, and the tools SHALL not be registered there.

#### Scenario: Windows package
- **WHEN** the Windows package is produced
- **THEN** it contains no acecode-ios-sim and no ios_simulator tools are offered
