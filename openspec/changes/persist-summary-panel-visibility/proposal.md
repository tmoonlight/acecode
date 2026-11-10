## Why

The summary panel currently opens by default. Wide windows remember the toolbar choice, while narrow windows lose it when the session reloads.

## What Changes

- Start with the summary closed when a profile has no saved value.
- Remember opening and closing the panel in the same local preference across sessions and window sizes.

## Capabilities

### Modified Capabilities

- `webui-side-panel` gains a requirement for the initial and saved visibility of the session summary.

## Impact

This affects Web and Desktop session layout, responsive popup handling, and frontend tests.
