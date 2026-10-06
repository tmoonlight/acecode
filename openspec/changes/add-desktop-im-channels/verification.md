# Verification: add-desktop-im-channels

Date: 2026-10-05. Main checkout `N:\Users\shao\acecode` on `master` (base `a67efe55`),
Windows 11 Enterprise 22631, MSVC Release, Ninja build directory
`N:\Users\shao\acecode-builds\im-channels` (`-DBUILD_TESTING=ON -DACECODE_BUILD_DESKTOP=ON`,
vcpkg `x64-windows-static`, curl 8.19 with the `websockets` feature).

## Automated checks (task 8.2)

- Build: `acecode`, `acecode-desktop` and `acecode_unit_tests` built successfully. The build
  reconfigured CMake and re-embedded the Web assets after `pnpm build`.
- Layer guards: `check_layers.py --layout final --strict`, `check_file_size.py --strict` and
  `check_ownership.py --strict --final` report 0 findings with the new untracked sources staged
  into a temporary index; `scripts/refactor/check_layer_libraries.py --build-dir` passes.
- Channel tests (transports, crypto, WebSocket, core, routes):
  `WebSocketClient*:AesGcm*:Im*:Qq*:Tg*:Telegram*:ChannelStore*:ChannelAccess*:ChannelCommands*:ChannelRouter*:ChannelOutbound*:ChannelHostTest*`
  — 197 tests, 3 shuffled repetitions, all passed. The core subset (64 tests) also passed 25
  shuffled repetitions; one race in a test's own wait condition was found this way and fixed.
- Web routes: `ChannelsHandler*`, `DaemonShutdownSequence*` and the complete `WebServer*` smoke
  suite (286 tests, which now constructs a channel host in every fixture) passed, including
  non-local rejection with a real `127.0.0.2` source address, credential masking, switch
  round-trip with a fake transport and the `channel_bound` session-list field.
- Full unit suite: 5765 tests from 836 suites in 496 s — 5756 passed, 0 failed, 9 skipped by design
  (opt-in real WhatsApp bridge/npm, native toast smoke, real image-generation and RSS network
  smokes, a local-only JSONL replay, and two POSIX-only tests). This run includes
  `QqTransport.OversizedFileBecomesTextNotice`, added after the repetition runs above.
- Frontend: `pnpm test` (3040 checks) and `pnpm build` passed; `pnpm i18n:catalog` regenerated the
  source catalog from reviewed English overrides (no machine translation).
- Daemon smoke: an isolated daemon (temporary `USERPROFILE`, `--run-dir`, `--port`) started with
  both channels unconfigured, served `GET /api/channels`, created no channel files, and exited
  cleanly through `acecode daemon stop` with no errors in its log. The settings page was checked
  in the in-app browser with unconfigured and offline-seeded (configured but switched off)
  Telegram data: cards, manual form, approvals, bound sessions and notes render in the open-group
  style.

End-to-end (`ImChannelsEndToEnd*`, 11 tests, 3 shuffled repetitions, all passed): the production
wiring `make_im_channel_host` with a real `SessionRegistry`/`AgentLoop` (scripted model) and the real
QQ and Telegram transports pointed at local fake platforms. Covered without real accounts:

- Telegram: wrong token rejected and not saved; real token verified by `getMe`; owner bound by the
  `/start` code from the owner link; private question answered in Telegram HTML; group needs approval
  and only @mentions are handled (privacy mode reported); `/sessions` + `/resume` into a workspace
  session and `/new` in that workspace; a non-owner cannot resume the owner's session; switching off
  stops processing immediately and switching on resumes from the saved offset; an inbound photo is
  downloaded into the session's attachments; a tool permission approved from Telegram, and a second
  one approved first from Desktop (`/approve` afterwards reports it was already handled; the tool runs
  once per request).
- QQ: QR binding against the fake portal (AES-GCM secret, owner from `user_openid`, switch turned on,
  gateway connected); private question answered as a passive reply with the original `msg_id`; voice
  recognition text enters the session; group and group member need separate approval and the member's
  @ question is answered in the group; an inbound image is downloaded with the bot token and imported;
  output refused by both passive and active sends is held and resent with `(补发)` before the next
  message is handled.

Issues found and fixed through these runs:

- Telegram reported "connected" only after the first `getUpdates` long poll returned; with no
  incoming messages a real bot would stay on "connecting" for up to 25 seconds after the switch is
  turned on. It is now connected as soon as `getMe` succeeds
  (`TelegramTransport.ReportsConnectedBeforeFirstLongPollReturns`).
- QQ files above the 20 MiB direct-upload limit were counted as failures without telling the
  contact; they are now replaced by a text notice, as the design requires
  (`QqTransport.OversizedFileBecomesTextNotice`).

Channel-related suites after these fixes (`WebSocketClient*:AesGcm*:Im*:Qq*:Tg*:Telegram*:Channel*:
ImChannelsEndToEnd*:DaemonShutdownSequence*:WebServerHttp.Channel*:WebServerHttp.SessionListMarks*:
RemoteControl*:SessionChannelBinder*`, including the existing WhatsApp channel tests): 411 tests,
407 passed, 4 skipped by design (opt-in real WhatsApp bridge/npm and image-generation network
smokes), 0 failed.

No test contacts Tencent or Telegram: transports run against local fake QQ (token, gateway
WebSocket, messages, uploads, bind portal) and Telegram (Bot API, long polling, multipart)
servers, and the QR bind flow uses a fixed key and an AES-GCM vector computed independently.

## Real-account acceptance (task 8.3)

Pending. It needs the operator's QQ and Telegram accounts on this Windows machine. The QQ QR flow
talks to `q.qq.com` and creates or binds a real bot, so it is only run together with the user.
Every item below except Desktop quit behavior already passes end to end against local fake
platforms (see above); the real-account run confirms the platforms' actual behavior, in particular
whether the QQ portal accepts `source=acecode`, real reply-window limits, and real media.
Checklist (record result and any limitation per item):

QQ (2026-10-05, same acceptance daemon; the user ran the QR flow on their own QQ account):
- [x] `source=acecode` QR binding: Tencent's binding portal accepted the bind page with source
      `acecode`; the scan created a bot, the secret was decrypted locally, QQ was
      switched on and connected, and the owner was set. Markdown is available for this bot.
- [x] private chat: "你好", "你是谁" and "你干嘛" from the owner were each answered in QQ; all three
      messages carry QQ channel metadata in their bound no-workspace session.
- [ ] group @mention
- [ ] approval in IM, and first-answer-wins with Desktop
- [ ] fallback after the passive reply window, held output and resend on the next message
- [ ] images and files in both directions
- [ ] voice recognition text

Telegram (2026-10-05, isolated acceptance daemon on 127.0.0.1:28931, real bot created with
@BotFather on the phone app):
- [x] token validation: the BotFather token was verified by `getMe` and saved; the platform
      connected and the page showed the bot's @username.
- [x] owner binding by QR link: the owner scanned the QR with the phone camera, tapped Start and
      became the owner. Finding: the page told the user to "scan with Telegram", but Telegram's
      built-in scanner only links devices and cannot read the link, so the user could not scan it.
      Fixed: the hint now says to use the phone camera, and the link next to the QR is clickable
      (opened in the system browser from Desktop) with a **Copy link** button; docs/channels.md and
      the help page say the same.
- [x] private chat: "你好", "请问你今天好吗", "你是谁" and "请问你有啥功能" from the owner were each
      answered in Telegram; all four messages carry Telegram channel metadata in their bound
      no-workspace session. (An earlier note here quoted the QQ session by mistake; corrected after
      checking each session's channel address.)
- [ ] group @mention, privacy-mode hint
- [ ] media in both directions

General:
- [ ] `/new` and `/sessions` + `/resume` into a workspace session
- [ ] non-owner switching is restricted
- [ ] sidebar icon follows the binding
- [ ] connect / disconnect take effect immediately
- [x] restart: after the acceptance daemon was stopped and started again, both platforms reconnected
      on their own and both bindings were restored (`[channels/*] binding ...` lines in the log).
- [ ] Desktop quit with "keep background process" on and off
- [ ] existing WhatsApp behavior unchanged

## Settings page redesign (acceptance feedback, 2026-10-05)

During acceptance the user found the settings page too detailed and asked for WorkBuddy's layout:
two columns of cards, a monochrome icon (default: chat), a **Connect** button that becomes
**Disconnect** once connected, a step-by-step dialog after **Connect**, and details printed to the
log instead of the page. Implemented as design D12 describes:

- Cards show only a one-line status and pending approval requests; **Fix** reopens the connect
  step after a failure, **Manage** changes the bot or revokes contacts.
- **Connect** opens a three-step dialog (set up bot, connect, set owner) when credentials or the
  owner are missing, and connects directly otherwise. The QQ step fetches the QR code on open;
  the Telegram owner step explains to scan with the phone camera and offers the clickable link and
  **Copy link**.
- `src/host/channels/core/state_log.cpp` logs snapshot changes at every `publish_state`: state and
  reason, platform extras (privacy mode, webhook, held outputs), owner and owner window, approved
  contacts, pending requests, bindings and send counts. Credentials (even masked) and QR content are
  never logged. Inbound decisions (with `AccessResult::reason`, no message text), IM commands, QQ
  binding phases and send fallbacks are logged where they happen.
- Corrected the Telegram spec: with group privacy mode on (the BotFather default) @mentions are
  still delivered; checked against Telegram's documentation and BotFather's own description.

Checks: `ChannelStateLog*` (6 new tests) and the extended `ChannelAccess.GroupNeedsApprovalThenMemberApprovalAndMention`;
channel-related suites 413 passed, 4 skipped by design, 0 failed; ownership (`--strict --final`), layer
and file-size checks report 0 findings with the new files staged; `pnpm test` 3044 passed,
including the guard that the page renders none of the logged details; `pnpm build` passed. After
the restart the daemon log showed the new lines, for example
`[channels/telegram] group privacy mode on: groups deliver only @mentions, replies to the bot and commands`.

## More platforms: WeChat, Feishu, DingTalk, Discord, LINE (2026-10-06)

Automated checks on Windows (Ninja, `acecode-builds/im-channels`), all against local fake platforms:

- Build: `acecode_unit_tests` and `acecode` built with the five new transports
  (`src/adapters/im/{weixin,feishu,dingtalk,discord,line}/`) and `core/line_webhook_server`.
- Channel and platform suites (`Channel*`, `ImChannelsEndToEnd`, `Qq*`, `Telegram*`, `Weixin*`,
  `Feishu*`, `DingTalk*`, `Discord*`, `Line*`, `Im*`, `WebSocketClient`, `AesGcm`, `Crypto*`,
  `ChannelsHandler`): 485 passed, 0 failed on the final build.
- `ImChannelsEndToEnd`: 16 passed, including the five new ones that go through the production
  wiring (`make_im_channel_host`, real `SessionRegistry` and scripted model):
  WeChat scan login → owner from the scanner → private chat with the contact's `context_token`;
  Feishu, DingTalk, Discord and LINE credentials validated → connected → 6-digit owner code →
  private chat (Feishu replies to the message, DingTalk through the session webhook, Discord in the
  DM channel, LINE with the free reply token and no push).
- `pnpm test` 3048 passed (including LINE connect hints and WeChat re-scan when the login expired);
  `pnpm build` passed; i18n catalog has no missing entries.
- Ownership (`--strict --final`), layer and file-size checks: 0 findings with the new files staged.
- `openspec validate add-desktop-im-channels --strict` passed; `git diff --check` reports no
  whitespace errors.
- Found while checking: the single-file compile checker used during drafting compiled its template
  instead of the target, so earlier "compiles" were unverified; after fixing it a missing-brace bug
  in `PlatformRuntime::set_credentials` showed up and was fixed before the build above.

Not yet verified with real accounts (to do with the user): WeChat QR login and expiry; Feishu long
connection, group @ delivery and post rendering; DingTalk Stream registration, @name stripping in
group text and image rendering; Discord resume, DM opening and the 4014 message; LINE stateless
tokens, Use webhook after an API URL change, quick-tunnel reachability, reply-token window,
push quota and image fetching through the tunnel.

## Known limits

- Only Windows was built and run; macOS and Linux are expected to compile (OpenSSL EVP for
  AES-GCM, `flock` owner lock) but were not built in this pass.
- QQ guild channels, chunked uploads, typing status, Telegram webhooks and hosted bots are out of
  scope; held QQ outputs live in memory only.
- Feishu and DingTalk hand each event to one random connection of the app: the same app running in
  another tool silently takes part of the messages. `WebSocketClient` exposes no handshake response
  headers, so Feishu's 403 / connection-limit handshakes are retried. DingTalk has no WebSocket ping
  of its own and reconnects after 120 s without frames.
- LINE stays "connecting" until Use webhook is turned on by hand; it cannot send files (images go
  through the listener's temporary links), and cloudflared is not tied to a kill-on-close job, so a
  daemon crash can leave it running. Quick tunnels change address on every start.
- Discord forum channels, Feishu card buttons and quoted-message text, and Discord rate-limit
  pacing from response headers are not implemented.
