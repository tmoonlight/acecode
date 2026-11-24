## Why

Selecting OpenCode Go from models.dev fills the inference URL, but ACECode drops the catalog identity when constructing HTTP headers. Go rejects otherwise valid model calls with HTTP 400 MissingSessionID.

## What Changes

Apply Go's documented session and client identification to all supported inference transports using the selected provider identity. Recognize official Go URLs for older profiles. Preserve configured header overrides and keep the generated values out of saved configuration.

## Capabilities

### New Capabilities

- `provider-session-headers` supplies provider-specific routing headers for inference requests.

## Impact

Affects HTTP model providers, model connection tests and side-chat request options. No change to the settings form or daemon wire format.
