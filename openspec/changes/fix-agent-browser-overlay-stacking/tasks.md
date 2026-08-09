## 1. Overlay Contract

- [x] 1.1 Harden document overlay discovery so registered visible floating surfaces remain authoritative over the Browser placeholder.
- [x] 1.2 Normalize and merge simultaneous overlap rectangles before sending native layout.
- [x] 1.3 Audit existing floating-surface roots and register any missing menus, popovers, toasts, and modal surfaces.

## 2. Reliable Native Delivery

- [x] 2.1 Track native layout acknowledgements by revision and add whole-surface fallback when local occlusion delivery fails.
- [x] 2.2 Keep macOS clipping and hit testing correct for multiple simultaneous occlusion rectangles.
- [x] 2.3 Correct macOS mask Y conversion for flipped backing layers so visual holes match DOM top coordinates.

## 3. Verification

- [x] 3.1 Add focused frontend tests for authoritative overlays, rectangle merging, and acknowledgement fallback.
- [x] 3.2 Extend macOS smoke coverage for multiple overlay holes and restored input.
- [x] 3.3 Run web tests/build, macOS smoke verification, and build/restart the Desktop application.
- [x] 3.4 Add a non-symmetric mask-path assertion, rebuild, rerun smoke verification, and restart Desktop.
