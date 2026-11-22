## Implementation

- [x] 1.1 Resolve provider session headers centrally from catalog identity, endpoint, configured headers and per-call identity.
- [x] 1.2 Use the resolver in synchronous and streaming transports and preserve the detached conversation identity.
- [x] 1.3 Add regression coverage for HTTP headers, connection tests and profile isolation, document the behavior and run verification.

## Verification

- Windows Release passed 134 focused GoogleTests covering provider construction, HTTP transports, connection testing, side chat, request building and cache identity.
- An isolated daemon selected `glm-5.3-flash` from its own `opencode-go` catalog and returned HTTP 200 with `ok: true` from `/api/models/test` against the official Go endpoint, with no custom headers.
- Three headless invocations against a local capture server verified the wire session ID survives process restart with `--resume` and changes for a new conversation.
- A live headless conversation with the official Go `glm-5.3-flash` endpoint completed a native `file_read` call and returned a random code. After removing the sample file and restarting ACECode with `--resume` and no tools, the model returned the same code from the conversation history.
- Strict layer, file-size and ownership checks passed.
