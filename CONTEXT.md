# ACECode

ACECode is a local AI coding agent with terminal, desktop, daemon, and headless runtime surfaces. This context records product terms that distinguish those surfaces and their diagnostic artifacts.

## Language

**Feedback origin**:
The runtime surface that initiates a diagnostic feedback package: `tui` for the terminal command and `desktop` for the GUI/Desktop endpoint.
_Avoid_: Feedback client, UI type

**Surface log**:
The dated runtime log produced by one ACECode surface, named with that surface's prefix in the effective data directory's `logs` subdirectory.
_Avoid_: Shared UI log, workspace log
