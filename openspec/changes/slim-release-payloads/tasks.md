## 1. Compressed frontend

- [x] 1.1 Implement deterministic compressed resource generation and verify binary/control-byte round trips and incremental regeneration tests (2 Python tests passed).
- [x] 1.2 Serve negotiated gzip with identity fallback and verify MIME, caching, q=0 handling and concurrent lookups in native tests (focused native suite and 8 real HTTP checks passed; actual bundled page started in Edge).

## 2. Shared pixel expert identities

- [x] 2.1 Extract the existing office character renderer and identity helpers; verify the same seed produces identical portrait and office pixels (unit tests, 9 avatar browser checks, 70 office regression checks, native-page inline-byte verification).
- [x] 2.2 Carry expert/member identity through session and office projections and subagent UI; verify team lead/member/repeated-session associations and custom avatar preservation (C++ office HTTP/catalog identity test, pure JS and browser tests passed).
- [x] 2.3 Replace built-in expert PNG resources with pixel defaults and update the seed manifest; verify fresh install, managed upgrade and user-modified package preservation (native seed suite passed, including both new portrait migration cases).

## 3. macOS update deduplication

- [x] 3.1 Build the small bundled CLI forwarder and verify paths, arguments, exit status and missing-payload failures with focused tests (3 real POSIX compiler/exec and package tests passed on WSL).
- [x] 3.2 Package the app once plus the forwarding entry; update package/signature/resource checks and verify the old flat updater contract and desktop app discovery (real POSIX launcher/package tests, Windows old-flat-apply test and existing Mac bundle tests passed; native Apple trust checks remain in release CI).

## 4. Integration and evidence

- [x] 4.1 Run web tests/build, focused native tests/build, script tests, strict OpenSpec validation and scoped diff checks; record exact results and any unavailable native platform checks (all applicable Windows/WSL/browser gates passed; native Mac trust checks are explicitly pending the normal release environment).
- [x] 4.2 Verify rendered expert/subagent/office identity, gzip HTTP/browser startup and before/after payload sizes; document the final behavior and measured savings (see design.md validation record and the daemon API/release-artifact documentation).
