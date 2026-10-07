// 消息通道设置页(设置 > 集成 > 消息通道,openspec add-desktop-im-channels)的纯逻辑:
// 快照规整、卡片(连接 / 取消连接 + 一行简要状态)、分步连接向导各步的视图、扫码流程视图、
// WS 事件合并、二维码路径、会话列表的 channel_bound 判定。组件 components/ChannelsSettings.jsx
// 只负责渲染与请求。页面刻意只给简要状态:原因、隐私模式、暂存待补发、收发计数等细节由 daemon
// 写进日志(src/host/channels/core/state_log.cpp),不要再加回页面。
import qrcode from 'qrcode-generator';

// 与 daemon 的平台描述表(src/host/channels/core/platforms.cpp)同序。
export const CHANNEL_PLATFORMS = ['qq', 'weixin', 'feishu', 'dingtalk', 'telegram', 'discord', 'line'];

// 有扫码绑定流程的平台。
export const SCAN_PLATFORMS = ['qq', 'weixin'];

function text(value) {
  return value == null ? '' : String(value);
}

function object(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function array(value) {
  return Array.isArray(value) ? value : [];
}

function count(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

// 平台短名(侧栏提示“已绑定到 …”、请求通知用)。写成函数:文案在调用时求值,跟随界面语言。
export function channelPlatformTitle(platform) {
  switch (platform) {
    case 'qq': return 'QQ';
    case 'weixin': return '微信';
    case 'feishu': return '飞书';
    case 'dingtalk': return '钉钉';
    case 'telegram': return 'Telegram';
    case 'discord': return 'Discord';
    case 'line': return 'LINE';
    default: return text(platform);
  }
}

export function isChannelPlatform(platform) {
  return CHANNEL_PLATFORMS.includes(platform);
}

export function emptyChannelPlatform(platform) {
  return {
    platform,
    configured: false,
    enabled: false,
    state: 'unconfigured',
    detail: '',
    retryStopped: false,
    account: '',
    displayName: '',
    extra: {},
    credentialHint: '',
    credentialsPublic: {},
    appId: '',
    owner: '',
    ownerWindow: false,
    hostedByPid: 0,
    contacts: [],
    pending: [],
    bindings: [],
  };
}

export function normalizeChannelPlatform(raw) {
  const source = object(raw);
  const platform = text(source.platform);
  const base = emptyChannelPlatform(platform);
  return {
    ...base,
    configured: source.configured === true,
    enabled: source.enabled === true,
    state: text(source.state) || base.state,
    detail: text(source.detail),
    retryStopped: source.retry_stopped === true,
    account: text(source.account),
    displayName: text(source.display_name),
    extra: object(source.extra),
    credentialHint: text(source.credential_hint),
    credentialsPublic: object(source.credentials_public),
    appId: text(source.app_id),
    owner: text(source.owner),
    ownerWindow: source.owner_window === true,
    hostedByPid: count(source.hosted_by_pid),
    contacts: array(source.contacts).map((item) => {
      const contact = object(item);
      return {
        principal: text(contact.principal),
        kind: text(contact.kind),
        name: text(contact.name),
        owner: contact.owner === true,
      };
    }).filter((contact) => contact.principal),
    pending: array(source.pending).map((item) => {
      const request = object(item);
      return {
        id: text(request.id),
        kind: text(request.kind),
        name: text(request.name),
        principal: text(request.principal),
        label: text(request.label),
        expiresInS: count(request.expires_in_s),
      };
    }).filter((request) => request.id),
    bindings: array(source.bindings).map((item) => {
      const binding = object(item);
      return {
        key: text(binding.key),
        label: text(binding.label),
        chat: text(binding.chat),
        sender: text(binding.sender),
        sessionId: text(binding.session_id),
        noWorkspace: binding.no_workspace !== false,
        cwd: text(binding.cwd),
        workspaceHash: text(binding.workspace_hash),
        sent: count(binding.sent),
        failed: count(binding.failed),
        dropped: count(binding.dropped),
      };
    }).filter((binding) => binding.key && binding.sessionId),
  };
}

// 扫码绑定流程的状态(QQ、微信)。
export function normalizeBind(raw) {
  const source = object(raw);
  return {
    platform: text(source.platform),
    phase: text(source.phase) || 'idle',
    qrUrl: text(source.qr_url),
    refreshes: count(source.refreshes),
    error: text(source.error),
    account: text(source.account),
    ownerBound: source.owner_bound === true,
  };
}

export function normalizeChannelsSnapshot(raw) {
  const source = object(raw);
  const platforms = {};
  for (const platform of CHANNEL_PLATFORMS) platforms[platform] = emptyChannelPlatform(platform);
  for (const item of array(source.platforms)) {
    const normalized = normalizeChannelPlatform(item);
    if (isChannelPlatform(normalized.platform)) platforms[normalized.platform] = normalized;
  }
  const rawBinds = object(source.binds);
  const binds = {};
  for (const platform of SCAN_PLATFORMS) binds[platform] = normalizeBind({ platform, ...object(rawBinds[platform]) });
  return { platforms, binds };
}

// WS 事件合并:channels_state 替换单个平台快照,channels_bind 更新该平台的扫码状态;
// 无关事件原样返回同一个对象(调用方可据此跳过重渲染)。
export function applyChannelsEvent(state, envelope) {
  const message = object(envelope);
  const payload = object(message.payload);
  if (message.type === 'channels_state') {
    const platform = normalizeChannelPlatform(payload);
    if (!isChannelPlatform(platform.platform)) return state;
    return { ...state, platforms: { ...state.platforms, [platform.platform]: platform } };
  }
  if (message.type === 'channels_bind' && SCAN_PLATFORMS.includes(payload.platform)) {
    return { ...state, binds: { ...object(state.binds), [payload.platform]: normalizeBind(payload) } };
  }
  return state;
}

// 平台卡片的固定内容。品牌图标由 ChannelPlatformIcon 使用本地 SVG 提供。
// 写成函数:文案在渲染时求值,切换界面语言后跟着变。
export function channelCards() {
  const card = (platform, title, description) => ({ platform, title, description });
  return [
    card('qq', 'QQ 机器人', '在 QQ 私聊或群里 @机器人 下达指令,结果回到聊天窗口。'),
    card('weixin', '微信', '在微信里私聊机器人下达指令,结果回到聊天窗口。'),
    card('feishu', '飞书', '在飞书私聊或群里 @机器人 下达指令,结果回到聊天窗口。'),
    card('dingtalk', '钉钉', '在钉钉私聊或群里 @机器人 下达指令,结果回到聊天窗口。'),
    card('telegram', 'Telegram', '在 Telegram 私聊或群里 @机器人 下达指令,结果回到聊天窗口。'),
    card('discord', 'Discord', '私信机器人或在服务器频道里 @它 下达指令,结果回到频道。'),
    card('line', 'LINE', '在 LINE 私聊或群里 @机器人 下达指令,结果回到聊天窗口。'),
  ];
}

const DISCORD_PERMISSIONS = '274878008320';  // 查看频道、发消息、读历史、附件、在子区发消息

export function discordInviteUrl(applicationId) {
  const id = text(applicationId);
  return id ? `https://discord.com/oauth2/authorize?client_id=${id}&scope=bot&permissions=${DISCORD_PERMISSIONS}` : '';
}

// 每个平台的连接向导:三步的名称、第一步(扫码或填写凭据)的说明与字段、连上后要回平台后台
// 完成的事、机主怎么产生(scan:QQ 扫码人 / scanner:微信扫码人 / link:Telegram 链接 /
// code:6 位绑定码)、完成时的提示。
export function platformSetup(platform) {
  const field = (key, label, extra = {}) => ({ key, label, placeholder: label, secret: false, ...extra });
  switch (platform) {
    case 'qq':
      return {
        steps: ['扫码创建机器人', '连接', '绑定机主'],
        credential: {
          kind: 'scan',
          scanHint: '扫码后会自动创建机器人并完成配置,扫码人成为机主。',
          manualIntro: '填写 QQ 开放平台里机器人的 AppID 与 AppSecret,保存前会先验证。',
          fields: [field('app_id', 'AppID'), field('app_secret', 'AppSecret', { secret: true })],
        },
        connectChecklist: [],
        owner: 'scan',
        doneHint: '现在可以在 QQ 里私聊机器人了。把机器人拉进群后 @ 它,第一次需要在设置页批准。未经个人认证的机器人只能由你本人使用,也只能加入你担任群主的群。',
      };
    case 'weixin':
      return {
        steps: ['扫码登录', '连接', '完成'],
        credential: {
          kind: 'scan',
          scanHint: '用微信扫一扫并在手机上确认;扫码的微信号就是机主,也是唯一能使用它的人。',
          manualIntro: '',
          fields: [],
        },
        connectChecklist: [],
        owner: 'scanner',
        doneHint: '现在可以在微信里给机器人发消息了。微信机器人只支持私聊。',
      };
    case 'feishu':
      return {
        steps: ['创建应用', '连接', '绑定机主'],
        credential: {
          kind: 'form',
          instructions: [
            '打开飞书开放平台,创建「企业自建应用」,在「添加应用能力」里添加「机器人」。',
            '在「权限管理」开通:im:message、im:message:send_as_bot、im:message.p2p_msg:readonly、im:message.group_at_msg:readonly、im:resource。',
            '在「凭证与基础信息」复制 App ID 与 App Secret,填到下面。',
          ],
          links: [{ label: '打开飞书开放平台', href: 'https://open.feishu.cn/app' }],
          fields: [
            field('app_id', 'App ID', { placeholder: 'App ID(cli_ 开头)' }),
            field('app_secret', 'App Secret', { secret: true }),
            field('domain', '版本', {
              kind: 'select',
              options: [{ value: 'feishu', label: '飞书(中国)' }, { value: 'lark', label: 'Lark(海外)' }],
              defaultValue: 'feishu',
            }),
          ],
        },
        connectChecklist: [
          '回到飞书开放平台的「事件与回调」,订阅方式选「使用长连接接收事件」并保存(这里连上之后才能保存)。',
          '添加事件「接收消息 v2.0」(im.message.receive_v1)。',
          '在「版本管理与发布」创建版本并发布,可用范围要包含你自己。',
        ],
        owner: 'code',
        ownerHint: '在飞书里搜索这个机器人,私聊它发送下面的 6 位数字。',
        doneHint: '现在可以在飞书里私聊机器人了。把机器人拉进群后 @ 它,第一次需要在设置页批准。',
      };
    case 'dingtalk':
      return {
        steps: ['创建应用', '连接', '绑定机主'],
        credential: {
          kind: 'form',
          instructions: [
            '打开钉钉开放平台,创建「企业内部应用」,在「应用能力」里添加「机器人」。',
            '机器人的消息接收模式选「Stream 模式」,在「权限管理」开通企业内机器人发送消息权限,然后发布应用。',
            '在「凭证与基础信息」复制 Client ID 与 Client Secret,填到下面。',
          ],
          links: [{ label: '打开钉钉开放平台', href: 'https://open-dev.dingtalk.com/' }],
          fields: [field('client_id', 'Client ID'), field('client_secret', 'Client Secret', { secret: true })],
        },
        connectChecklist: [],
        owner: 'code',
        ownerHint: '在钉钉里找到这个机器人,私聊它发送下面的 6 位数字。',
        doneHint: '现在可以在钉钉里私聊机器人了。把机器人拉进群后 @ 它,第一次需要在设置页批准。',
      };
    case 'telegram':
      return {
        steps: ['创建机器人', '连接', '绑定机主'],
        credential: {
          kind: 'form',
          instructions: [
            '在 Telegram 里搜索 @BotFather(认准蓝色认证标记),发送 /newbot。',
            '按提示起名字,用户名要以 bot 结尾。',
            '把它回复的 Token 粘贴到下面。',
          ],
          links: [],
          fields: [field('token', 'Bot Token', { secret: true })],
        },
        connectChecklist: [],
        owner: 'link',
        doneHint: '把机器人拉进群后 @ 它,第一次需要在设置页批准。',
      };
    case 'discord':
      return {
        steps: ['创建机器人', '连接', '绑定机主'],
        credential: {
          kind: 'form',
          instructions: [
            '打开 Discord 开发者后台,新建应用,在「Bot」页点「Reset Token」并复制。',
            '在同一页打开「Message Content Intent」。',
            '把 Token 粘贴到下面;验证通过后用邀请链接把机器人拉进你的服务器。',
          ],
          links: [{ label: '打开 Discord 开发者后台', href: 'https://discord.com/developers/applications' }],
          fields: [field('token', 'Bot Token', { secret: true })],
        },
        connectChecklist: [],
        owner: 'code',
        ownerHint: '在 Discord 里私信这个机器人(需要和它在同一个服务器),发送下面的 6 位数字。',
        doneHint: '现在可以私信机器人,或在服务器频道里 @ 它。第一次在频道里使用需要在设置页批准。',
      };
    case 'line':
      return {
        steps: ['创建频道', '连接', '绑定机主'],
        credential: {
          kind: 'form',
          instructions: [
            '在 LINE Official Account Manager 建立官方账号,并在「设定 > Messaging API」启用 Messaging API。',
            '在 LINE Developers 控制台的频道「Basic settings」复制 Channel ID 与 Channel secret,填到下面。',
            '在「Messaging API」页打开 Use webhook;在官方账号的回应设定里关闭自动回应讯息与加入好友的欢迎讯息。',
          ],
          note: 'LINE 只能通过公网地址回调。公网地址留空时,ACECode 会用 cloudflared 自动建立临时隧道(先安装:winget install --id Cloudflare.cloudflared),每次连接都会自动更新 LINE 的回调地址。',
          links: [
            { label: '打开 LINE Developers', href: 'https://developers.line.biz/console/' },
            { label: '打开 LINE Official Account Manager', href: 'https://manager.line.biz/' },
          ],
          fields: [
            field('channel_id', 'Channel ID'),
            field('channel_secret', 'Channel secret', { secret: true }),
            field('public_url', '公网地址', { placeholder: '公网地址(可选,留空则自动建立隧道)', clearable: true }),
          ],
        },
        connectChecklist: [],
        owner: 'code',
        ownerHint: '在 LINE 里把官方账号加为好友,私聊它发送下面的 6 位数字。',
        doneHint: '现在可以在 LINE 里私聊机器人了。把机器人邀请进群后 @ 它,第一次需要在设置页批准。',
      };
    default:
      return {
        steps: ['配置', '连接', '绑定机主'],
        credential: { kind: 'form', instructions: [], links: [], fields: [] },
        connectChecklist: [],
        owner: 'code',
        ownerHint: '',
        doneHint: '',
      };
  }
}

export function wizardSteps(platform) {
  return platformSetup(platform).steps;
}

// 已保存的机器人在向导与管理对话框里的一行摘要(只用非密钥字段与密钥尾号)。
export function savedBotSummary(platformKey, platform) {
  const item = platform || emptyChannelPlatform(platformKey);
  const fields = object(item.credentialsPublic);
  const value = (key) => text(fields[key]);
  switch (platformKey) {
    case 'qq':
      return { title: `AppID ${item.appId || value('app_id') || '未知'}`, desc: `AppSecret ${item.credentialHint || '已保存'}` };
    case 'weixin':
      return { title: item.displayName || '微信机器人', desc: value('bot_id') ? `机器人 ${value('bot_id')}` : '' };
    case 'feishu':
      return { title: item.displayName || `App ID ${value('app_id')}`, desc: `App Secret ${value('app_secret') || '已保存'}` };
    case 'dingtalk':
      return { title: item.displayName || `Client ID ${value('client_id')}`, desc: `Client Secret ${value('client_secret') || '已保存'}` };
    case 'line':
      return { title: item.displayName || `Channel ${value('channel_id')}`, desc: `Channel secret ${value('channel_secret') || '已保存'}` };
    default:
      return { title: item.displayName || `${channelPlatformTitle(platformKey)} 机器人`, desc: `Token ${item.credentialHint || '已保存'}` };
  }
}

// 卡片:按钮(连接 / 取消连接)与一行简要状态,tone 取 ok / warn / danger / mute。
// 失败只说“详情见日志”,并给出 action:'fix' 让卡片显示「处理」入口(打开向导的连接步)。
export function channelCardView(platform, busy = false) {
  const item = platform || emptyChannelPlatform('');
  const button = item.enabled
    ? { action: 'disconnect', label: '取消连接', disabled: Boolean(busy) }
    : { action: 'connect', label: '连接', disabled: Boolean(busy) || item.state === 'error' };
  return { button, status: cardStatus(item) };
}

function cardStatus(item) {
  if (item.state === 'error') return { tone: 'danger', text: '通道数据异常,详情见日志' };
  if (!item.enabled) return null;
  switch (item.state) {
    case 'connected':
      return { tone: 'ok', text: item.displayName ? `已连接 · ${item.displayName}` : '已连接' };
    case 'connecting':
      return { tone: 'warn', text: '连接中…' };
    case 'retrying':
      return { tone: 'warn', text: '连接中断,正在自动重试' };
    case 'failed':
      return telegramWebhookBlocked(item)
        ? { tone: 'warn', text: '机器人设置了 webhook,需要处理', action: 'fix' }
        : { tone: 'danger', text: '连接失败,详情见日志', action: 'fix' };
    case 'standby':
      return { tone: 'mute', text: '已由另一个 ACECode 连接' };
    default:
      return { tone: 'mute', text: '未连接' };
  }
}

// 点「连接」:还没配好(没有凭据,或还没有机主)时打开分步向导,已经配好的直接连接。
export function connectNeedsWizard(platform) {
  const item = platform || emptyChannelPlatform('');
  return !item.configured || !item.owner || scanLoginExpired(item);
}

// 微信的登录凭据会在腾讯侧失效:平台停止重试后只能重新扫码,重连旧凭据没有意义。
function scanLoginExpired(item) {
  return item.platform === 'weixin' && item.state === 'failed' && item.retryStopped;
}

// 向导从哪一步开始:没有凭据(或微信登录已失效)从第 1 步,有凭据从第 2 步(连接)。
export function initialWizardStep(platform) {
  if (!platform?.configured || scanLoginExpired(platform)) return 0;
  return 1;
}

// 向导第 2 步(连接)的视图:phase 决定能做什么,text 是给用户看的一句话。
export function connectStepView(platform) {
  const item = platform || emptyChannelPlatform('');
  if (item.state === 'error') return { phase: 'failed', tone: 'danger', text: '通道数据文件异常,详情见日志。' };
  if (item.state === 'connected') {
    return { phase: 'connected', tone: 'ok', text: item.displayName ? `已连接 ${item.displayName}` : '已连接' };
  }
  if (!item.enabled) return { phase: 'idle', tone: 'mute', text: '正在打开连接…' };
  const lineHint = lineConnectHint(item);
  if (lineHint) return lineHint;
  switch (item.state) {
    case 'failed':
      if (telegramWebhookBlocked(item)) {
        return {
          phase: 'webhook',
          tone: 'warn',
          text: '这个机器人设置了 webhook,ACECode 收不到消息。移除后由 ACECode 接管,原来接收 webhook 的服务会停止收到消息。',
        };
      }
      return { phase: 'failed', tone: 'danger', text: '连接失败,原因已写入日志。确认凭据无误后重试,或回到上一步更换。' };
    case 'retrying':
      return { phase: 'connecting', tone: 'warn', text: '连接中断,正在自动重试…' };
    case 'standby':
      return { phase: 'standby', tone: 'mute', text: '这个机器人正由另一个 ACECode 连接,它退出后这里会自动接管。' };
    default:
      return { phase: 'connecting', tone: 'mute', text: '正在连接…' };
  }
}

// LINE 连接里只有用户能做的几步:装 cloudflared、在控制台打开 Use webhook、把自己的公网地址
// 转发到本机回调端口。其余失败原因照常只写日志。
function lineConnectHint(item) {
  if (item.platform !== 'line') return null;
  const extra = item.extra || {};
  if (item.state === 'failed' && extra.cloudflared_missing === true) {
    return {
      phase: 'failed',
      tone: 'danger',
      text: '没有找到 cloudflared。先运行 winget install --id Cloudflare.cloudflared 安装后重试,或回到上一步填写自己的公网地址。',
    };
  }
  if (item.state !== 'retrying') return null;
  if (extra.webhook_active === false) {
    return {
      phase: 'connecting',
      tone: 'warn',
      text: '回调地址已登记。请在 LINE Developers 控制台的 Messaging API 页打开 Use webhook,打开后这里会自动变为已连接。',
    };
  }
  const port = Number(extra.listen_port) || 0;
  if (extra.tunnel === 'custom' && port > 0 && !text(extra.webhook_url)) {
    return {
      phase: 'connecting',
      tone: 'warn',
      text: `请把你的公网地址转发到本机 http://127.0.0.1:${port},连通后会自动继续。`,
    };
  }
  return null;
}

export function ownerTitle(platform) {
  const owner = text(platform?.owner);
  if (!owner) return '';
  const contact = array(platform?.contacts).find((item) => item.principal === owner);
  return contact?.name || principalLabel(owner);
}

// 向导第 3 步(绑定机主)。Telegram 用一次性链接;飞书、钉钉、Discord、LINE 用 6 位绑定码;
// QQ 扫码时通常已带出机主,没带出时靠 10 分钟窗口,窗口过了就批准对方的私聊请求
// (批准第一个私聊请求即成为机主);微信的扫码人就是机主。
export function ownerStepView(platform, platformKey) {
  const item = platform || emptyChannelPlatform(platformKey);
  if (item.owner) return { done: true, mode: 'done', text: `机主:${ownerTitle(item)}` };
  const owner = platformSetup(platformKey).owner;
  if (owner === 'link') return { done: false, mode: 'link', text: '' };
  if (owner === 'code') return { done: false, mode: 'code', text: platformSetup(platformKey).ownerHint };
  const title = channelPlatformTitle(platformKey);
  if (item.ownerWindow) {
    return { done: false, mode: 'window', text: `接下来 10 分钟内第一个私聊机器人的人成为机主,现在就用你的 ${title} 私聊它一句。` };
  }
  return { done: false, mode: 'request', text: `用你的 ${title} 私聊机器人一句,下面会出现请求,批准后你就是机主。` };
}

// 扫码流程视图(QQ、微信)。
export function bindView(bind, platformKey = 'qq') {
  const state = bind || normalizeBind(null);
  const active = state.phase === 'starting' || state.phase === 'waiting';
  switch (state.phase) {
    case 'starting':
      return { active, showQr: false, tone: 'mute', message: '正在获取二维码…' };
    case 'waiting':
      return {
        active,
        showQr: Boolean(state.qrUrl),
        tone: 'mute',
        message: state.refreshes > 0
          ? '二维码过期后已自动换新,请扫描新的二维码'
          : platformKey === 'weixin'
            ? '用微信扫一扫,并在手机上确认'
            : '用手机 QQ 扫一扫,按提示创建并授权机器人',
      };
    case 'completed':
      return { active, showQr: false, tone: 'ok', message: state.ownerBound ? '扫码成功,你已是机主' : '扫码成功' };
    case 'failed':
      return {
        active,
        showQr: false,
        tone: 'danger',
        message: state.error || (platformKey === 'weixin' ? '获取二维码失败,请重试' : '获取二维码失败,可以重试,或改为手动填写'),
      };
    case 'cancelled':
      return { active, showQr: false, tone: 'mute', message: '已取消扫码,原有配置未改动' };
    case 'timed_out':
      return { active, showQr: false, tone: 'danger', message: '二维码已过期' };
    default:
      return { active: false, showQr: false, tone: 'mute', message: '' };
  }
}

export function principalLabel(principal) {
  const value = text(principal);
  if (value.startsWith('member:')) {
    const rest = value.slice('member:'.length);
    const split = rest.indexOf(':');
    if (split > 0) return `群 ${rest.slice(0, split)} 的成员 ${rest.slice(split + 1)}`;
  }
  if (value.startsWith('group:')) return `群 ${value.slice('group:'.length)}`;
  if (value.startsWith('user:')) return `用户 ${value.slice('user:'.length)}`;
  return value;
}

export function principalKindLabel(kind) {
  if (kind === 'group') return '群';
  if (kind === 'member') return '群成员';
  return '私聊';
}

export function contactRows(platform) {
  return array(platform?.contacts).map((contact) => ({
    ...contact,
    title: contact.name || principalLabel(contact.principal),
    subtitle: contact.name ? principalLabel(contact.principal) : '',
    kindLabel: contact.owner ? '机主' : principalKindLabel(contact.kind),
  }));
}

export function pendingRows(platform) {
  return array(platform?.pending).map((request) => {
    const minutes = Math.max(1, Math.ceil(request.expiresInS / 60));
    return {
      ...request,
      title: request.name || principalLabel(request.principal),
      subtitle: [principalKindLabel(request.kind), request.label, `${minutes} 分钟后过期`]
        .filter(Boolean).join(' · '),
    };
  });
}

export function telegramWebhookBlocked(platform) {
  return object(platform?.extra).webhook === true;
}

// 平台第一步要填写的字段(QQ 是扫码之外的手动填写)。
export function credentialFields(platform) {
  return platformSetup(platform).credential.fields;
}

// 保存凭据的请求体:去两端空白,空字段省略(表示沿用已保存的值);标了 clearable 的可选字段
// 留空表示清除(如 LINE 公网地址改回自动隧道)。没有任何内容时返回 null。
export function credentialsPayload(platform, draft) {
  const source = object(draft);
  const body = {};
  let filled = false;
  for (const field of credentialFields(platform)) {
    const value = text(source[field.key] ?? field.defaultValue).trim();
    if (value) {
      body[field.key] = value;
      if (field.kind !== 'select') filled = true;
    } else if (field.clearable) {
      body[field.key] = '';
    }
  }
  return filled || Object.keys(body).some((key) => body[key] === '') ? body : null;
}

// 输入框占位:已保存的密钥给尾号并提示留空不改,已保存的普通字段给原值,否则给字段名。
export function credentialPlaceholder(platform, field) {
  const saved = text(object(platform?.credentialsPublic)[field.key]);
  if (saved) return field.secret ? `已保存 ${saved},留空则不修改` : `${saved}(留空则不修改)`;
  return field.placeholder || field.label;
}

// 二维码:返回带 4 格静区的边长与 SVG 路径(按行合并相邻深色模块)。
export function qrSvgModel(content, errorCorrection = 'M') {
  const value = text(content);
  if (!value) return null;
  const code = qrcode(0, errorCorrection);
  code.addData(value);
  code.make();
  const modules = code.getModuleCount();
  const quiet = 4;
  const parts = [];
  for (let row = 0; row < modules; row += 1) {
    let col = 0;
    while (col < modules) {
      if (!code.isDark(row, col)) {
        col += 1;
        continue;
      }
      let end = col;
      while (end < modules && code.isDark(row, end)) end += 1;
      parts.push(`M${col + quiet} ${row + quiet}h${end - col}v1h-${end - col}z`);
      col = end;
    }
  }
  return { size: modules + quiet * 2, modules, path: parts.join('') };
}

// 会话列表:被 IM 会话绑定的会话带 channel_bound:{platform};返回平台名或空串。
export function channelBoundPlatform(session) {
  const bound = object(session?.channel_bound ?? session?.channelBound);
  const platform = text(bound.platform);
  return isChannelPlatform(platform) ? platform : '';
}

export function channelBoundLabel(platform) {
  return platform ? `已绑定到 ${channelPlatformTitle(platform)}` : '';
}

// 所有平台绑定的会话 id 集合(排序后的字符串,便于比较是否变化)。
export function channelBindingSignature(state) {
  const ids = [];
  for (const platform of CHANNEL_PLATFORMS) {
    for (const binding of array(state?.platforms?.[platform]?.bindings)) ids.push(`${platform}:${binding.sessionId}`);
  }
  return ids.sort().join('|');
}

// 单个 channels_state 事件里的绑定签名;侧栏据此判断要不要立即刷新。
export function channelStateBindingSignature(payload) {
  const platform = normalizeChannelPlatform(payload);
  if (!isChannelPlatform(platform.platform)) return null;
  return platform.bindings.map((binding) => binding.sessionId).sort().join('|');
}

// 接口错误的展示文案:优先用后端给的中文 message(不含凭据);远程客户端统一提示本机设置。
export function channelErrorText(error) {
  const body = object(error?.body);
  if (error?.code === 'LOCAL_ONLY' || body.error === 'LOCAL_ONLY') return '消息通道只能在运行 ACECode 的这台电脑上设置';
  return text(body.message) || text(error?.message) || '操作失败';
}
