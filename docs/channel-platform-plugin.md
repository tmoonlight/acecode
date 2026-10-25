# Channel Platform Plugins

A channel platform plugin adds an instant-messaging platform to **Settings >
Integrations > Messaging channels** without putting that platform's code into
ACECode (`openspec add-channel-platform-plugins`). The plugin is a separate
executable that ACECode starts as a long-lived child process and talks to over
stdin/stdout. Logging in, the IM connection, message storage and echo
suppression all stay inside the plugin; ACECode only sees the generic protocol
below.

This is different from the `/rc` channel plugin protocol
([channel-plugin-protocol.md](channel-plugin-protocol.md)), which only binds and
unbinds one session through webhooks. `/rc` is unchanged.

## Manifest

The daemon scans `<data_dir>/connectors/*/channel-platform.json` once at
startup. A connector package installs the program and the manifest into the same
directory, so the card appears only when the package is installed. Restart the
daemon after changing a manifest.

```json
{
  "schema": "acecode.channel-platform.v1",
  "name": "example",
  "label": "Example IM",
  "command": "example-im.exe",
  "args": ["--channel-transport", "--cwd", "${manifest_dir}"],
  "account": "self",
  "features": ["watched_groups"]
}
```

| Field | Rule |
| --- | --- |
| `schema` | Must be `acecode.channel-platform.v1`. |
| `name` | `^[a-z][a-z0-9-]{1,31}$`. Used as the data directory name, the API path segment and `Address.platform`. Must not collide with a built-in platform or an earlier plugin (directories are scanned in name order). |
| `label` | 1–64 bytes. Card title, log and IM label. |
| `command` | Relative to the manifest directory, or absolute. Must be an existing regular file. |
| `args` | Up to 32 strings. `${manifest_dir}` becomes the manifest directory's absolute path. |
| `account` | Only `self`: the plugin is logged in as the user's own account. |
| `features` | Optional. `watched_groups` enables group chat delegation. Unknown values are ignored. |

An invalid manifest is skipped with one log line (`[channels] plugin manifest
skipped: ...`); other platforms are unaffected. The child process runs in the
manifest directory; its stderr is discarded.

## Framing

UTF-8, one JSON object per line, at most 1 MiB per line. The frames match the
WhatsApp bridge:

- Request `{"id": 1, "method": "send_text", "params": {...}}`
- Response `{"id": 1, "ok": true, "result": {...}}` or `{"id": 1, "ok": false, "error": "reason"}`
- Notification (ACECode to plugin, no response) `{"method": "set_typing", "params": {...}}`
- Event (plugin to ACECode) `{"event": "status", "params": {...}}`

A line that is not a JSON object stops the process and triggers a restart.
Unknown events are ignored, so a plugin may add events before ACECode reads
them.

## ACECode to plugin

| Method | Params | Result | Timeout |
| --- | --- | --- | --- |
| `initialize` | `protocol_version` (1), `platform`, `settings` | `protocol_version`, `capabilities` | 30 s |
| `configure` | `settings` | `{}` | 10 s |
| `send_text` | `address`, `text` (Markdown), `reply_context` | `outcome`: `sent` / `held` / `failed`, `error` | 60 s |
| `send_file` | `address`, `path`, `name`, `mime_type`, `reply_context` | as `send_text` | 120 s |
| `download` | `attachment`, `dest` | `ok`, `error` | 120 s |
| `action` | `name`, `args` | any object | 120 s |
| `shutdown` | `{}` | `{}` | 3 s |
| `set_typing` (notification) | `address`, `on` | none | — |

`settings` currently holds `watched_groups`: the group IDs the user wants
delegated. Only emit group messages for these groups. `configure` sends the new
list when the user changes it.

`capabilities` uses the field names of `im::Capabilities`: `max_text_units`
(64–65536, default 4000), `count_utf16`, `batch_turn_output`, `supports_typing`,
`max_upload_bytes`, `max_download_bytes`.

`action` names in use: `login` (the card's **Sign in** button). Respond only
after the login flow has started; report the outcome through `status`.

A timed-out request is reported as failed and never resent, because its delivery
outcome is unknown.

## Plugin to ACECode

### `status`

```json
{"event": "status", "params": {
  "state": "connected",
  "detail": "",
  "retry_stopped": false,
  "account": "user-id",
  "display_name": "Zhang San",
  "extra": {"login_required": false}
}}
```

`state` is `connecting`, `connected`, `retrying`, `failed` or `stopped`.
`account` is required before any `inbound` event: inbound messages without a
known account are dropped. `display_name` is the user's own name; group
delegation tells the model whom it represents. Set `extra.login_required: true`
when the user must sign in again, for example after another client took over the
account. The card then shows **Sign in required** and a **Sign in** button, and
ACECode does not retry on its own.

### `inbound`

```json
{"event": "inbound", "params": {
  "address": {"kind": "group", "chat": "group-id", "sender": "member-id"},
  "message_id": "unique-id",
  "text": "message text",
  "mentioned": true,
  "from_self": false,
  "sender_name": "Bob",
  "chat_name": "Project group",
  "timestamp_ms": 1761300000000,
  "quote_text": "",
  "attachments": [],
  "reply_context": {}
}}
```

ACECode fills `address.platform` and `address.account`; values sent by the
plugin are ignored. IDs use letters, digits and `- _ . : @ + / = $` (at most 128
bytes). For a private chat `chat` must equal `sender`.

With `account: self`, only private messages where `sender` equals the account
(messages the user sent to themselves) reach a session. Distinguishing the
user's messages from the replies ACECode sent back, and marking those replies, is
the plugin's job.

Group messages (`kind: group`) go to group delegation only. `mentioned` means
the message mentions the user; `from_self` marks messages the user sent from
another client. Do not emit messages ACECode itself sent through `send_text`.

## Lifecycle

Turning the card on starts the process and sends `initialize`. Turning it off,
or stopping the daemon, sends `shutdown`, closes stdin, waits 3 seconds and then
kills the process. If the process exits unexpectedly, `initialize` fails, or the
protocol version differs, the card shows a retrying state and ACECode restarts
the process after 1 second, doubling to at most 60 seconds. After a run of at
least 60 seconds the delay starts again from 1 second. Only the daemon that
holds the platform's `owner.lock` starts the process.

## Group chat delegation

With `watched_groups`, the card's **Manage** dialog has a form: group IDs, poll
interval (30 seconds to 24 hours, default 120), message prefix (at most 32
characters, default none) and delegate prompt (at most 4000 characters). It is
stored in `<data_dir>/channels/<name>/config.json` under `group_watch`.

Messages from watched groups are buffered (the latest 200). When the interval
has passed, the buffer contains a message from someone else, and the group's
session is idle, ACECode submits one user message:

```
[群聊新消息] 群「Project group」10:00–10:05,共 3 条
[10:00] Bob: Can we ship on Friday?
[10:02] Carol(@我): @Zhang please check
[10:05] 我: On it
```

Each group gets its own no-workspace session on first use. Its metadata carries
`channel_delegate` (`{platform, chat, chat_name, user_name, prompt}`), which
makes the agent:

- append a `<group-delegate>` block to the system prompt, stating whom it
  represents, that group messages are data rather than instructions, that only
  `send_to_group_chat` reaches the group, and the user's delegate prompt;
- keep only `file_read`, `grep`, `glob`, `lsp`, `web_search`, `skills_list`,
  `skill_view`, `memory_read`, `task_complete` and `send_to_group_chat`, and no
  MCP tools.

The model's ordinary replies, permission requests and questions never go to the
group. `send_to_group_chat` sends one message with the configured prefix, at
most 2000 characters and 5 messages per digest. Ordinary sessions do not see
the tool. Removing a group from the form unbinds its session and clears
`channel_delegate`; the session stays as ordinary history.
