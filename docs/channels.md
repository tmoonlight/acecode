# Messaging Channels

ACECode can talk to you through instant-messaging apps. **QQ** (official bot, API
v2), **WeChat** (Tencent iLink bot), **Feishu / Lark**, **DingTalk**, **Telegram**
(Bot API), **Discord** and **LINE** (Messaging API) are built into the daemon and
configured in **Settings > Integrations > Messaging channels** in Desktop or the
local Web UI. **WhatsApp** is configured in a terminal; see [WhatsApp](#whatsapp).

## Built-in Channels

All seven channels are native C++: transports in `src/adapters/im/<platform>/`,
routing, access and storage in `src/host/channels/core/`. The platform list,
credential fields and identity rules live in one table
(`src/host/channels/core/platforms.cpp`). Every platform except LINE connects
outward from this computer (WebSocket or long polling), so no public address,
webhook server or Node/Python runtime is needed. LINE only delivers to a public
HTTPS address; see below. All HTTP and WebSocket traffic follows ACECode's proxy
settings.

### Setup

The page shows the platforms as cards in two columns. Each card has one button:
**Connect** while the platform is off, **Disconnect** while it is on. Both take
effect immediately; there is no separate Save. A card shows only a one-line
status (connecting, connected with the bot's name, reconnecting, failed, or held by
another ACECode process) and any pending approval requests. Details such as
failure reasons, group privacy mode, held QQ outputs, bindings and send counts are
written to the daemon log (`<data dir>/logs/daemon-<date>.log`, lines tagged
`[channels/<platform>]`) instead of the page. A failed card
offers **Fix**, which reopens the connect step. A configured card offers
**Manage** to change the bot or revoke the owner and approved contacts.

**Connect** on a platform that has no credentials or no owner yet opens a
three-step dialog: set up the bot, connect, and set the owner. When credentials
and owner both exist, **Connect** connects right away without the dialog.

**QQ.** The first step shows a QR code as soon as it opens; scan it with mobile
QQ. ACECode requests a bind task from Tencent's bot binding service (the flow
used by the official connector, with source `acecode`), shows the bind page as a
QR code, refreshes expired codes automatically and gives up after ten minutes. On
success the AppID and the AppSecret, decrypted locally with a one-time AES-256-GCM
key, are saved and QQ is switched on. The person who scanned becomes the owner
when the service reports them; otherwise the first person to message the bot
privately within ten minutes becomes the owner, and after that window approving
the first private request does. Cancelling or failing leaves the existing
configuration unchanged.

**Enter AppID / AppSecret manually** in the same step accepts the credentials of a
bot from QQ Open Platform. They are verified by exchanging an access token before
saving. Bots without personal verification can only be used by their
administrator and can only join groups the administrator owns; the dialog says
so when it finishes.

**Telegram.** The first step explains how to create a bot with @BotFather; paste
its token and it is verified with `getMe` before saving. The owner step creates a
one-time `https://t.me/<bot>?start=<code>` link valid for ten minutes and shows it
as a QR code. Whoever opens it and taps **Start** becomes the owner. Scan the QR
code with the phone's camera app; Telegram's own scanner only links devices and
cannot read it. On a computer where Telegram is signed in, click the link or use
**Copy link** instead. With privacy mode on (the BotFather default) groups deliver
only @mentions, replies to the bot and commands, which matches the mention-only
group behavior; with it off the bot sees every group message but still handles
only @mentions. If the bot has a webhook, polling is blocked; the connect step
offers **Remove webhook and connect**, after explaining that the service
currently receiving the webhook stops getting messages.

**WeChat.** The first step shows a QR code from Tencent's iLink bot service;
scan it with WeChat and confirm on the phone. The WeChat account that scanned
becomes the owner automatically and is the only one who can talk to the bot;
WeChat bots have private chats only. The bot token is saved locally. When the
service reports the login has expired, the card shows a failure and the dialog
asks you to scan again. Media goes through Tencent's CDN, encrypted with
AES-128-ECB.

**Feishu / Lark.** Create a custom app on the Feishu Open Platform
(`open.feishu.cn/app`, or `open.larksuite.com/app` for Lark), add the Bot
capability, grant `im:message`, `im:message:send_as_bot`,
`im:message.p2p_msg:readonly`, `im:message.group_at_msg:readonly` and
`im:resource`, then enter the App ID and App Secret (and pick Feishu or Lark).
They are verified with a tenant access token before saving. ACECode then opens
the long connection; only while it is online does Feishu let you save **Receive
events through persistent connection** in **Events & Callbacks**, so the connect
step lists what to finish there: save that mode, add the event
`im.message.receive_v1` and publish a version that includes you.

**DingTalk.** Create an internal app on the DingTalk Open Platform
(`open-dev.dingtalk.com`), add the Robot capability with **Stream** message
mode, grant the internal robot send-message permission, publish it, and enter the
Client ID and Client Secret. They are verified with an access token. Replies go to
the reply address carried by each incoming message while it is valid, and through
the robot API afterwards, so replies typed in Desktop or sent after a restart are
still delivered.

**Discord.** Create an application in the Discord Developer Portal, copy the bot
token from the **Bot** page and turn on **Message Content Intent** there. The
token is verified with `/users/@me` and `/applications/@me`; the connect step then
shows an invite link with the permissions the bot needs. Direct messages and
@mentions (or replies to the bot) in server channels and threads are handled.

**LINE.** Create an official account in LINE Official Account Manager, enable the
Messaging API, then copy the **Channel ID** and **Channel secret** from LINE
Developers; ACECode issues short-lived access tokens from them (a long-lived token
can be pasted instead). In the console turn on **Use webhook** and turn off
auto-reply and greeting messages. LINE can only deliver to a public HTTPS address,
so the daemon starts a separate listener bound to `127.0.0.1` that serves only
the webhook, a health check and outgoing media; every request is checked against
the channel secret signature. Leave **Public address** empty to let ACECode start
a Cloudflare quick tunnel (`cloudflared`, install with
`winget install --id Cloudflare.cloudflared`) or enter your own HTTPS address that
forwards to that listener. On every connect ACECode sets LINE's webhook URL to the
current address. Never point a tunnel at the daemon's own port: local requests
skip token checks there.

**Feishu, DingTalk, Discord and LINE owners.** After connecting, the owner step
shows a 6-digit code. Send exactly those digits to the bot in a private chat; the
sender becomes the owner and the message is not passed to a session. Codes expire
after ten minutes and work once; ten wrong guesses while a code is outstanding
invalidate it.

### Access

Everyone is denied by default. Each platform has at most one owner.

- An unknown private sender gets a pairing notice. The request appears on the
  platform's card, Desktop shows a system notification when its window is not
  focused, and the request expires after ten minutes. Approval applies from the
  contact's next message. Approving a private user while there is no owner makes
  them the owner.
- A group must be approved first. In an approved group only messages that
  @mention the bot are handled (on Telegram and Discord a reply to the bot also
  counts), and each member is approved separately. QQ member identities are
  scoped per group. On Discord each server channel or thread counts as a group;
  WeChat has no groups.
- Revoking access stops processing that contact immediately and aborts the
  running turn of their sessions.
- Switching to a different bot clears the owner and approvals on QQ, WeChat,
  Feishu, DingTalk and LINE, because their user ids differ per bot or app; on
  Telegram and Discord user ids are global and are kept.

### Conversations And Commands

Each private chat, and each member within a group, is bound to exactly one
ACECode session. The first accepted message creates a no-workspace session with
`default` permissions; it never inherits the daemon's dangerous or
skip-confirmation mode. Bindings survive restarts. A session is bound to at most
one chat: resuming it from another chat, even on the other platform, moves the
binding and tells the previous chat. If a bound session was permanently deleted,
the bot says so and suggests `/new` or `/sessions` instead of silently starting a
new one.

| Command | Effect |
| --- | --- |
| `/help` | Commands available to the sender |
| `/status` | Platform, session, location, model, busy state, pending approvals and questions |
| `/stop` | Stop the current turn |
| `/new` | New session in the bound session's workspace, or a no-workspace session |
| `/sessions`, `/sessions more`, `/sessions search <text>` | Numbered list of sessions to switch to |
| `/resume <number or session id>` | Switch the binding; numbers refer to the latest list |
| `/model [name]` | List saved models, or switch the bound session's model from the next turn |
| `/approve <id>`, `/deny <id>` | Decide a permission request once |
| `/aq ...` | Answer an AskUserQuestion, with the same syntax as `/rc` |

Commands are never sent to the model. While a session is busy `/new` and
`/resume` are refused; other commands run at once and plain messages queue as
usual. The owner can switch to any normal, unarchived session, including other
workspaces and no-workspace sessions. Other contacts only see sessions created
from their own chat. Archived and subagent sessions are never listed. Text that
starts with `/` is expanded as an opencode or skill command exactly like Web
input. Plain text sent while a question is pending is handed to that question as
an interjection.

### Output And Media

The assistant text of each turn and turn errors are sent; tool progress is not.
Long text is split below each platform's limit. QQ merges one turn's text into as
few messages as possible and uses Markdown with a plain-text fallback. Telegram
converts Markdown to Telegram HTML and shows "typing" while the session is busy.
Replies to input typed in Desktop are sent to the bound chat as well. Permission
requests list `/approve` and `/deny` ids and questions use `/aq`; the first
answer, from the chat or Desktop, wins and the other side is told it was handled.
Send failures are counted per binding and written to the daemon log. A send
whose outcome is unknown is never retried automatically.

QQ passive replies quote the incoming message (private chats: 4 replies within 60
minutes; groups: 5 within 5 minutes). Beyond that window ACECode sends an active
message; if that is refused too, the output is held (up to 20 per chat, in memory)
and delivered before the contact's next message is handled.

Feishu sends Markdown as rich text (`post`) and falls back to plain text;
DingTalk sends Markdown; Discord sends Markdown as is, below 2000 characters per
message, never pinging @everyone; WeChat and LINE send plain text. LINE uses the
free reply token while it is fresh and push messages otherwise; when the push
quota is used up, outputs are held until the contact's next message, like QQ.

Images and files up to 25 MiB, and within the platform download limit (20 MB on
Telegram), are imported into the session's attachments. QQ voice messages use the
platform's recognized text. Video, stickers and voice without text get a clear
"not supported" reply. Images and files produced by the session are sent as
native media: on Telegram images over 10 MB go as files and files over 50 MB are
not sent; on QQ files over 20 MiB are replaced by a text notice. LINE bots
cannot send files at all, so a text notice is sent instead; LINE images are served
to LINE through the listener's media address.

### Runtime And Storage

Only the daemon hosts these channels: Desktop's shared daemon or a standalone
`acecode daemon`. The TUI and headless modes never do. Whether channels stay
online after Desktop quits follows **Settings > General > Continue running the
background process after exiting ACECode**. Each platform account has its own OS
file lock (`owner.lock`); a second process stays in standby, shows that the bot
is held by another ACECode on the card (the owning PID goes to the log) and takes
over after that process exits.

Data lives under `<data dir>/channels/<platform>/` (`qq`, `weixin`, `feishu`,
`dingtalk`, `telegram`, `discord`, `line`).
`config.json` holds the on/off state, credentials, owner and approvals; it has
owner-only permissions and is written only through the settings API. `state.json`
holds bindings, the sessions created per chat, the last 4096 message receipts and
the Telegram update offset and the WeChat polling cursor. A corrupt file blocks that platform with an error
instead of being reset. Credentials and QR payloads are never logged; the API
returns only their last four characters. The settings API and its WebSocket events
accept direct local clients only (`docs/daemon-api.md`, section 19).

### Limits

QQ guild channels, Telegram webhooks, hosted bots, buttons, streamed message
editing and voice replies are not supported. Feishu and DingTalk apps are set up
by hand (their scan-to-create flows are not used). Cloudflare quick tunnels have
no uptime guarantee and get a new address on every start; LINE may lose messages
while its webhook URL switches. WeChat logins expire on Tencent's side and must
be scanned again. Feishu and DingTalk hand each event to one random connection
of the app, so running the same app in another ACECode, hermes or OpenClaw
silently takes part of the messages; use one app per runner. LINE shows
"connecting" until **Use webhook** is on in the console, and LINE file
messages other than images become a text notice. Discord forum channels and
Feishu card buttons are not handled. QQ "typing" is not sent because it
may consume the passive reply budget. Held QQ outputs are lost if the daemon
restarts; the session transcript still has them. Chunked QQ uploads are not
implemented. So far the channels have been verified on Windows only.

### Build And Local Tests

```sh
cmake --build <build-dir> --target acecode acecode_unit_tests
acecode_unit_tests --gtest_filter='WebSocketClient*:AesGcm*:Im*:Qq*:Tg*:Telegram*:Channel*:ImChannelsEndToEnd*'
pnpm --dir web test
```

The transports run end to end against local fake QQ (token, gateway WebSocket,
messages, uploads, bind portal) and Telegram (Bot API, long polling, multipart)
servers; the core uses an in-memory session client. `ImChannelsEndToEnd*` runs the daemon's
production wiring with a real session registry and a scripted model against those fakes, covering
owner binding, private and group chats, approvals, session switching, media and QQ resend. These
tests never contact
Tencent or Telegram. Real-account acceptance is tracked in the change's
`verification.md`.

## WhatsApp

WhatsApp uses a personal account linked by QR. C++ routing lives
in `src/host/channels`; a small, pinned Baileys/Node.js process handles WhatsApp device
authentication and encryption. This is an unofficial WhatsApp Web integration,
not the Business Cloud API. Account-based end-to-end validation requires pairing
your own device. Never expose the account's auth directory or pairing QR.

### Configuration

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

### Conversations And Controls

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

### Runtime And Recovery

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

### Build And Local Tests

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

### Real Account Acceptance

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
