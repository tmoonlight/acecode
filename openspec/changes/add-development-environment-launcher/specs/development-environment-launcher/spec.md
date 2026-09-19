# Spec Delta

## Purpose

Provide a safe, consistent way to start ACECode development surfaces across supported platforms without manually locating compatible build artifacts.

## ADDED Requirements

### Requirement: Development target selection
The development-environment launcher SHALL start exactly one selected development target: Web, Desktop, or TUI. When no target is provided to an interactive launcher invocation, it SHALL prompt the developer to select one. A repository-local assistant skill SHALL ask the developer to select one of those targets when a request to run the development environment does not name a target.

#### Scenario: Interactive target selection
- **WHEN** a developer starts the launcher without a target in an interactive terminal
- **THEN** the launcher prompts for Web, Desktop, or TUI and starts only the selected target

#### Scenario: Skill target selection
- **WHEN** a developer asks the repository-local skill to run the development environment without naming a target
- **THEN** the skill asks whether to run Web, Desktop, or TUI before starting work

### Requirement: Compatible build reuse
Before requesting a new configuration, the launcher and skill SHALL search the current worktree for a compatible existing build. A build is reusable only when its configured source directory is the current worktree, its executable is runnable on the current platform and architecture, and it contains the executable required by the selected target. A Desktop target additionally requires a Desktop-enabled build and Desktop executable. Other registered worktrees MAY provide content-addressed compiler cache entries and verified frontend artifacts, but their path-bound build directories and executables SHALL NOT be used for the current worktree.

#### Scenario: Reuse a matching Web build
- **WHEN** the current worktree has a compatible configured `acecode` build
- **THEN** the launcher incrementally builds and starts the Web target with that build directory

#### Scenario: Reject an incompatible Desktop build
- **WHEN** a matching build lacks Desktop support or the Desktop executable
- **THEN** the launcher does not use it for the Desktop target

### Requirement: Fresh incremental builds and configuration confirmation
Before starting a selected target from a compatible build directory, the launcher SHALL run that target's incremental CMake build so changed source files are incorporated. Before starting Web or Desktop, it SHALL also ensure the development frontend assets are current. When no compatible configured build exists, the launcher and skill SHALL report the missing requirement and the CMake preset selected for the current platform, then configure the development target with testing disabled so optional unit-test dependencies do not block startup. They SHALL obtain explicit developer confirmation before configuring a new build unless a Windows direct entry point supplies its automatic approval. A declined confirmation SHALL leave source and build files unchanged and SHALL not start a development target.

#### Scenario: Refresh a compatible build
- **WHEN** a compatible build exists and source files have changed
- **THEN** the launcher runs the selected target's incremental build before starting it

#### Scenario: Confirm a required configuration
- **WHEN** the selected target has no compatible build and the developer confirms the proposed configuration
- **THEN** the launcher configures and incrementally builds the required target before starting it

#### Scenario: Decline a required configuration
- **WHEN** the selected target has no compatible build and the developer declines the proposed configuration
- **THEN** the launcher exits without configuring, compiling, or starting a target

### Requirement: Windows direct-launch configuration
Windows target-specific batch entry points SHALL pass automatic configuration approval to the shared launcher. When no compatible configured build exists, those direct entry points SHALL configure and build it without requiring console input. The shared Python launcher and POSIX direct entry points SHALL retain explicit confirmation requirements for a missing build.

#### Scenario: Double-clicked Web launcher requires a first build
- **WHEN** a developer starts the Windows Web batch entry point and no compatible configured build exists
- **THEN** it configures, builds, and starts the Web target without waiting for confirmation input

### Requirement: Windows compiler environment initialization
Before a Windows target-specific entry point runs an incremental CMake build, it SHALL initialize an installed Visual Studio C++ developer environment for the host and target architecture. If no suitable Visual Studio C++ tools installation is available, it SHALL exit before invoking the shared launcher and report how to install the required Build Tools workload.

#### Scenario: Start from a normal Windows shell
- **WHEN** a developer starts a Windows target-specific entry point from a shell without Visual Studio compiler variables
- **THEN** the entry point initializes the developer environment and the incremental build receives the C++ standard-library include paths

#### Scenario: Missing Visual Studio C++ tools
- **WHEN** a Windows target-specific entry point cannot locate a Visual Studio C++ developer command script
- **THEN** it reports the missing Build Tools prerequisite and does not start a build

### Requirement: Target-specific startup
The launcher SHALL reuse the repository's existing Web and Desktop launch scripts for those targets. It SHALL start TUI in a new terminal window. Web startup SHALL use a runtime directory isolated to the current worktree and SHALL open the resulting local Web URL after successful startup. Desktop startup SHALL open the Desktop application after successful startup.

#### Scenario: Isolated Web startup
- **WHEN** a developer starts the Web target for a worktree
- **THEN** its daemon uses a worktree-specific runtime directory and opens that daemon's URL

#### Scenario: TUI startup
- **WHEN** a developer starts the TUI target
- **THEN** the launcher opens the TUI executable in a new terminal window

### Requirement: Target-specific cross-platform entry points
The repository SHALL provide dedicated thin Windows and POSIX entry points for Web, Desktop, and TUI. Each entry point SHALL select its target without requiring a target argument, delegate to the shared development-environment launcher, pass supported command-line arguments through unchanged, and provide a clear error when no supported Python interpreter is available.

#### Scenario: Windows Web launch
- **WHEN** a Windows developer runs the Web batch entry point
- **THEN** it delegates to the shared launcher with the Web target selected

#### Scenario: POSIX TUI launch
- **WHEN** a macOS or Linux developer runs the TUI shell entry point
- **THEN** it delegates to the shared launcher with the TUI target selected
