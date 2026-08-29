## 1. Final Native Geometry Policy

- [x] 1.1 Add a pure helper that fits and centers an actual outer-window size inside an absolute DPI-aware work-area rectangle
- [x] 1.2 Add regression tests for bottom taskbar reservation, high-DPI post-WebView scaling, non-zero monitor origins, and degenerate work areas

## 2. Windows First-Show Integration

- [x] 2.1 Re-query the selected monitor work area and apply the final actual-HWND fit immediately before first display
- [x] 2.2 Apply the same first-show correction to the WebView-owned fallback path and keep later hide/show behavior unchanged

## 3. Verification and Delivery

- [x] 3.1 Run focused unit tests, strict OpenSpec validation, and a Windows Desktop build
- [x] 3.2 Verify a real Desktop startup window remains inside the live work area and above the taskbar at scaled DPI
- [ ] 3.3 Commit and push the scoped repair, then publish and verify a Windows QuickValidation package
