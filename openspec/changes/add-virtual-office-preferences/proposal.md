## Why

Virtual Office needs an explicit, persistent opt-in and discoverable controls. The office cannot currently be reopened after closing, and late bridge registration can miss the initial macOS page load.

## What Changes

- Add a show/hide action between Settings and Appearance and a Virtual Office settings page between Personalization and Usage.
- Default to disabled. From 0.9.37, ask each user once with the supplied invitation and an animated, offline preview using the existing embedded office page.
- Share native preference and window lifecycle behavior between Windows and macOS; close, settings and menu update the same state.
- Register office bridges before navigation, stop session polling while hidden, and restore live session linkage when reopened.

## Capabilities

### New Capabilities
- `virtual-office-preferences`: persistent opt-in, entry points and one-time invitation.

### Modified Capabilities

## Impact

Desktop office hosts, main bootstrap, Web UI settings/menu/onboarding, existing embedded office preview. No new video assets, daemon protocol changes or release tagging.
