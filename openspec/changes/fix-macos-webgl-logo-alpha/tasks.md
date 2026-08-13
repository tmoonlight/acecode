## 1. Premultiplied Alpha Implementation

- [x] 1.1 Change the interactive-logo WebGL context and fragment shader to use premultiplied alpha end to end.
- [x] 1.2 Update architecture regression tests to assert the premultiplied context and direct premultiplied shader output.

## 2. Validation

- [x] 2.1 Run the Web test suite and production Web build.
- [x] 2.2 Reconfigure and build the macOS x64 desktop application with the updated embedded Web assets.
- [x] 2.3 Relaunch the desktop application and visually verify that light and dark themes no longer expose the WebGL canvas boundary.
- [x] 2.4 Validate the OpenSpec change in strict mode.
