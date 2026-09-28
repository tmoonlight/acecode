# WhatsApp Channels

The first channel is a personal WhatsApp account linked by QR. C++ routing lives
in `src/host/channels`; a small, pinned Baileys/Node.js process handles WhatsApp device
authentication and encryption. This is an unofficial WhatsApp Web integration,
not the Business Cloud API. Account-based end-to-end validation requires pairing
your own device. Never expose the account's auth directory or pairing QR.

## Configuration

Run **`acecode channels`** in an interactive terminal. `acecode channels setup`
is an explicit alias. This command only configures the account; it does not
start a daemon or leave a channel service running. There is no `/channels`
command or channel page inside the main TUI.

The standalone `acecode channels` command displays a content-sized wizard after
the shell command, without clearing existing terminal output or switching to a
full-screen buffer. Exiting returns below the wizard.

1. Choose **Just myself** (the default), or **Myself and selected contacts**.
2. For selected contacts, enter phone numbers with country codes, separated by
   commas. No WhatsApp JIDs are needed. Your own linked account is added automatically.
3. Choose **Save configuration**. An already linked account is reused without
   connecting or starting a process. First-time linking checks Node.js 22+ and
   installs pinned bridge dependencies automatically. It never starts a daemon.
   If Node.js/npm is missing or too old, use **Download Node.js**, install it,
   reopen ACECode if PATH changed, and retry. The wizard does not install Node.js
   or modify system settings itself.
4. Scan the live QR from **WhatsApp Settings > Linked devices > Link a device**.
   Expired codes refresh automatically; a saved login skips this step entirely.
   Enlarge the terminal if requested so the complete QR fits on screen.
5. After WhatsApp confirms the linked account, the wizard saves credentials,
   access and the auto-connect preference, then closes the temporary bridge.
   The completion screen only confirms that configuration is saved.

The saved configuration is read when **`acecode daemon`** or **ACECode Desktop**
next starts. Already-running instances are left alone, without checking their
status, applying changes to them, or asking the user to close or restart them.
Once connected, WhatsApp's **Message yourself** conversation can be used normally.

**Back**, **Cancel**, and **Retry** are available within the setup flow. Installation
and pairing run off the UI thread. Cancel stops pending installation/pairing; a
first-time configuration stays disabled until completion. Reconfiguration retains
existing access lists, credentials, conversation bindings, and histories. Pairing
uses a temporary configuration-only bridge that cannot send or deliver messages
to an agent. Success, cancellation and failure all stop that temporary process.
New pairing uses a unique credential profile and never opens another instance's
active credentials. Saving settings uses a separate, short configuration lock;
the runtime account lock does not block the wizard.
Pairing QR codes and setup progress never enter an agent conversation.

Dependencies and credentials live in the user data directory, outside Program
Files or a signed `.app`. No manual npm command is needed for normal first-time setup.

All other contacts are **denied by default**. With daemon/Desktop running, allow a known contact locally:

```text
acecode channels allow 15551234567@s.whatsapp.net
```

Or let an unknown contact send a DM, inspect `acecode channels pending`, and approve
their ten-minute pairing code with `acecode channels approve <code>`. Approval is only
available to the local operator; sending the code back over WhatsApp does not
grant access. The contact must send a new message after approval. Self-chat is
already authorized by the wizard. Bot output IDs are persisted to avoid self-chat
echo loops. Existing contacts remain authorized when the wizard is rerun; revoke
unwanted contacts explicitly with `acecode channels revoke <jid>`.

## Conversations And Controls

The first authorized message creates a regular **no-workspace session**, with
`default` tool permissions. It does not inherit a local skip-confirmations mode,
does not run inside the current terminal project, and is not a subagent. The
same contact resumes the same persisted session after restart. A group must be
allowlisted as well as its participant, and every input must explicitly mention
the linked account. Each group participant has their own session.

| Terminal command | Operation |
| --- | --- |
| `acecode channels [setup]` | Configure and link; do not start a service |
| `acecode channels status` | Live status, or saved configuration when no host is running |
| `acecode channels on`, `acecode channels reconnect` | Enable/restart in an already running host |
| `acecode channels off` | Disconnect, retain credentials and histories |
| `acecode channels qr` | Current pairing QR, without printing the raw login payload |
| `acecode channels pending`, `acecode channels approve <code>` | Inspect/approve incoming pairing requests |
| `acecode channels allow <jid>`, `acecode channels revoke <jid>` | Manage contact/group access |
| `acecode channels sessions` | Bound conversations and failed/dropped delivery counters |
| `acecode channels show <session-id>` | Recent persisted transcript and pending requests |
| `acecode channels send <session-id> <text>` | Submit input to that independent session |
| `acecode channels file <session-id> <path>` | Explicitly send a local file to its WhatsApp chat |
| `acecode channels stop <session-id>` | Stop its active turn |

Only configuration and status work without a running host. Other commands attach
to the current owner and report that daemon/Desktop must be started when absent;
none of these commands auto-start a background process.

Quote paths containing spaces. Local `file` is an explicit operator action and
can select a file outside the session; automatic outgoing files must resolve to
that session's stored attachments. Arbitrary Markdown file paths are not sent.

WhatsApp controls bypass the ordinary message queue, even while a tool waits:

```text
/status
/stop
/aq 1
/aq --status
/aq --cancel
/approve <permission-request-id>
/deny <permission-request-id>
```

`/aq` is the existing RC question interface, including multiple choice and free
text. The terminal can answer the same requests with `acecode channels send <session-id>
/aq ...` or `/approve ...`. Only the originating authorized participant or the
local operator can answer; the underlying question/permission prompter retains
first-wins semantics. Remote permission approval is one-time, not a persistent
grant. Other sessions' request IDs cannot be used.

Text, images and documents are accepted. Attachments are limited to 25 MiB and
imported into the existing session attachment store. Model support still applies
to image/document understanding. Native quoted replies preserve the incoming
message reference while it remains in the bounded bridge cache. Long text is
split using the existing RC helper. Unsupported media and delivery failures do
not terminate other conversations.

## Runtime And Recovery

Only the daemon worker hosts the gateway, using its existing SessionRegistry and
LocalSessionClient. Starting `acecode daemon` connects configured channels.
Desktop starts/reuses the same worker and connects without requiring a separately
started daemon. The main TUI does not host channels. Closing Desktop obeys its
existing background/keep-alive settings; a user-started daemon retains normal
daemon lifetime. `acecode channels off` stops communication without shutting
down unrelated sessions or deleting history. Configuration creates no dedicated
daemon, listener, agent session or `channels/whatsapp/run` runtime directory.

Each runtime reads its configuration once at startup. Disabled instances do not
claim account ownership or start a channel listener. An OS file lock is claimed
when the first enabled daemon/Desktop channel runtime starts.
Later instances remain standby and do not start another bridge, reconnect the
account, or consume its messages. They can take over only after the owner exits
and closes its bridge. A standby retains its startup configuration on takeover;
only conversation bindings and receipts are refreshed. Setup never acquires this lock.
The private `owner.json` descriptor publishes an
authenticated loopback endpoint only after readiness. This endpoint is separate
from the public daemon API and rejects browser-origin requests.

Settings and access live under `~/.acecode/channels/whatsapp/config.json`.
Conversation bindings and the last 4096 incoming receipts remain in `state.json`.
Legacy settings in `state.json` are read only if `config.json` does not exist, so
old running binaries cannot overwrite newly saved configuration. New credentials,
dependencies and media live under the selected `profiles/<id>/` directory;
legacy `auth/`, `bridge/` and `media/` directories remain usable without migration
or modification during setup. Pairing requests are memory-only and expire after
ten minutes. State corruption fails closed. A missing historical session is an
error, not an excuse to silently replace the binding with a new conversation.

Messages are recorded before dispatch to avoid repeated tool execution. A crash
between recording and enqueue may require the contact to send a **new** message.
Outbound delivery uses the bounded RC FIFO; it is not a durable outbox. Unknown
send outcomes are not automatically retried. Inspect `acecode channels sessions` for
failed/dropped sends and the session transcript before explicitly retrying.

No voice, scheduled/cross-channel delivery, other platforms, Matrix encryption,
Yuanbao, Signal, BlueBubbles or Photon support is included in this phase. Existing
`/rc` activation and bindings are unchanged.

## Build And Local Tests

`acecode` builds stage the bridge next to the executable. `cmake --install build
--config Release --prefix <package> --component whatsapp_channel` stages the
bridge assets for a flat package. macOS desktop builds include the same assets
under `Contents/Resources/channels/whatsapp`. npm dependencies are installed in
the user's private runtime directory on first setup, not bundled with the app.

```sh
cmake --build build --config Release --target acecode acecode_unit_tests
ctest --test-dir build -C Release --output-on-failure -R 'Channel'
npm ci --prefix assets/channels/whatsapp
npm test --prefix assets/channels/whatsapp
```

Local tests exercise mock transport/process failure, session routing, access,
attachments, questions, permissions, owner takeover, setup ordering, cancellation,
access preservation, QR rendering and wizard navigation. Set
`ACECODE_TEST_CHANNEL_INSTALL=1` to run the optional real npm installation test in
a temporary directory. It never starts WhatsApp or touches an account's credentials.
The real bridge smoke
test loads Baileys and checks the disconnected protocol without contacting an
account. These are not a substitute for scanning a QR and exchanging real
messages from the intended account.

## Real Account Acceptance

After pairing, verify the following with the intended account before relying on
unattended operation:

1. Fresh setup installs dependencies, refreshes the QR and saves only after
   linking succeeds. Completion leaves no bridge or background host, and messages
   are not processed until daemon/Desktop starts. Cancel before completion leaves
   it disabled. Rerunning setup retains existing contact access and conversations.
2. An unknown contact gets only a pairing code. Local approval permits a new
   message; revocation prevents further input and stops its active turn.
3. Two contacts get different no-workspace sessions. Self-chat does not feed bot
   replies back into the model. The current TUI session stays independent.
4. An allowed group ignores messages without a mention or from unapproved
   participants. Approved participants keep separate sessions.
5. Text, an image, a document and quoted replies arrive in the correct chat.
   Unsupported media returns a clear error.
6. `/aq`, `/approve`, `/deny` and `/stop` work while the session is busy. An answer
   from one surface closes the same pending request on the other surface.
7. Reconnect and restart retain the same session history. Opening Desktop alone
   reconnects an enabled account, and closing it follows the existing background
   preference. Starting another daemon/Desktop leaves the first connection alone;
   a standby takes over only after the owner exits.
