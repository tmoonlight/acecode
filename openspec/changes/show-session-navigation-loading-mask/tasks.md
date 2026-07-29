## 1. Session Navigation State

- [x] 1.1 Initialize shared pending-navigation state for runtime jumps and redirected startup targets.
- [x] 1.2 Wrap the shared resume/open flow with overlap-safe completion, failure cleanup, and redirect handoff behavior.

## 2. Full-Screen Loading Mask

- [x] 2.1 Add an accessible, themed full-viewport session navigation mask with an animated loading indicator.
- [x] 2.2 Render the mask at the application shell level for every pending session jump.

## 3. Verification

- [x] 3.1 Add focused frontend coverage for state wiring, redirect retention, startup behavior, and accessible mask presentation.
- [x] 3.2 Run focused tests, the full frontend suite, production build, strict OpenSpec validation, and diff checks.
