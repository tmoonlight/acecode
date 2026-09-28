<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

See proposal.md for scope. RC has a single global binding but already supplies a bounded outbound worker and question projection. SessionRegistry owns independent loops and no-workspace persistence. Desktop automatically runs the ordinary daemon worker. TUI's main session is a separate direct AgentLoop.

## Goals / Non-Goals

Keep session execution in the existing daemon registry and all new C++ protocol/control logic in src/channels. Preserve RC APIs. This phase does not replace the TUI main-session architecture or implement additional platforms.

## Decisions

- A channel runtime is assembled only in the daemon worker against its existing SessionClient/SessionRegistry. CLI diagnostics can attach to its authenticated control endpoint but never launch a host. Desktop's normal startup uses the same worker integration. The main TUI has no channel command, callback or surface.
- Each runtime reads configuration once at startup. Disabled instances do not claim channel ownership. An OS-backed exclusive account lock is acquired synchronously by the first enabled runtime and held for its lifetime, with an owner endpoint published only after readiness. Later enabled instances remain standby without starting a bridge. They may take over after the owner exits, retaining their startup configuration while refreshing conversation history. Shutdown stops ingress, unsubscribes sessions, drains outbound workers and closes the bridge before releasing the lock.
- Saved settings live in `config.json`, independently of owner-written bindings and receipt history in `state.json`. A short configuration-file lock serializes settings edits only; setup never checks runtime ownership or contacts a running host. Legacy state settings are read only when no configuration file exists, so old hosts cannot overwrite new settings. Settings never contain provider credentials. Baileys credentials stay in a private account directory selected by the startup configuration.
- The bridge uses bounded newline-delimited JSON request/reply and event frames over private stdio pipes. Reuse LspProcess for hidden, cross-platform child-process ownership. Baileys owns encryption, reconnect and QR events; C++ owns access decisions and sessions. No Python runtime is introduced.
- Each conversation binding subscribes to the existing session event stream and reuses an RC outbound hub plus ChannelQuestionBridge. PermissionRequest/PermissionClosed project existing AsyncPrompter requests into text controls. Subscribe before reading pending-request snapshots. Controls carry request identity and never enter the model queue.
- Stable account/contact identifiers form structured, unambiguous keys. Group keys include the participant. Bindings are persisted before first input; duplicate receipts are bounded and persisted. Resume explicitly requests no-workspace mode. Failure to resume must be visible rather than silently discarding history.
- Inbound media must reside in the bridge-owned media cache and meet size/type limits. Existing UserInput content parts carry images/documents. Outbound files use explicit channel controls or structured session attachments, checked against the session workspace/cache before loading. Native quote metadata is retained for replies.
- Terminal commands are a thin wrapper over channel control: status, on/off, qr, pairing/access, sessions, show, send, file, stop and request responses. They present operator-visible failures without credentials. Packaging installs four fixed bridge assets with the executable; runtime copies them into the private data directory and verifies installed npm versions against the packaged pins. Source checkout discovery is a development fallback.
- Channel creation and resume explicitly disable inheritance of the daemon's dangerous flag, independently of selecting default permissions. Normal sessions retain their existing permission behavior.

## Risks / Trade-offs

- WhatsApp protocol changes: pin and test Baileys; avoid reproducing its encryption/device code in C++.
- Process and late-callback races: single account ownership, weak binding subscriptions, explicit shutdown order and focused concurrency tests.
- External traffic cannot be fully verified without an operator scanning a QR: use real local protocol integration tests, and report account-based validation separately.
- User-started daemons follow normal daemon lifetime; a desktop-owned daemon follows existing desktop background settings. Configuration itself never creates either kind of host.

## Migration Plan

New installations default to disabled. Existing RC settings are untouched. Enabling creates channel-local state; disabling disconnects without deleting authentication or histories. Deploy the executable and matching bridge assets together; test clean package discovery and missing-dependency diagnostics.

## Guided Setup Follow-up

The initial command-only configuration did not satisfy the Hermes-style onboarding requirement. The standalone CLI provides a step-by-step wizard with `acecode channels` or `acecode channels setup`. There is no `/channels` integration inside the main TUI.

The standalone CLI wizard uses TerminalOutput instead of an alternate/full-screen buffer. It starts below the invoking shell command, sizes itself to the current step and leaves prior terminal output intact. The main TUI render settings remain unchanged. QR sizing uses available terminal dimensions, not the previous shorter step's drawing area.

The flow selects self-chat or additional allowed phone numbers and reuses an already saved login without connecting or preparing dependencies. If no linked login exists, it prepares pinned npm dependencies in a new private profile, displays a live QR, verifies the new linked account and flushes credentials before atomically selecting that profile and saving access. Completion reports only that configuration is saved. Phone numbers are normalized into JIDs internally. No Business Cloud API or extra platform is introduced.

A local setup session never acquires the runtime account lock or checks running applications. First-time pairing owns only a short-lived pairing-only bridge in a unique profile, never a Gateway, listener, agent session or daemon. Its bridge suppresses message events and rejects sending. Completion, cancellation, timeout and failure close its pairing process; unsuccessful profiles are discarded without touching existing credentials. Existing access and history are preserved. No busy-host, stop-host or restart instructions are displayed. UI work is asynchronous and keeps QR payloads out of chat history.
