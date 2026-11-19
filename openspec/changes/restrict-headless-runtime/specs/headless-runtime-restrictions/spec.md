## ADDED Requirements

### Requirement: headless-runtime-restrictions

The headless runtime SHALL expose and execute only explicitly permitted tools in restricted mode and replace generated instructions when configured.

#### Scenario: Normal operation

- **WHEN** An invocation configures a replacement file and a restricted MCP tool
- **THEN** Only that tool can register and every conversation request includes the operator instructions without local project context.
