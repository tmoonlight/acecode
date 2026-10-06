// lib/channelsSettings 单元测试:消息通道设置页的快照规整、卡片(连接 / 取消连接 + 简要状态)、
// 分步连接向导各步的视图、扫码视图、WS 事件合并、二维码路径与侧栏 channel_bound 判定。

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import qrcode from 'qrcode-generator';
import {
  applyChannelsEvent,
  channelBindingSignature,
  channelBoundLabel,
  channelBoundPlatform,
  channelCardView,
  channelCards,
  channelErrorText,
  channelStateBindingSignature,
  connectNeedsWizard,
  connectStepView,
  contactRows,
  credentialFields,
  credentialPlaceholder,
  credentialsPayload,
  discordInviteUrl,
  initialWizardStep,
  normalizeChannelsSnapshot,
  ownerStepView,
  pendingRows,
  principalLabel,
  bindView,
  platformSetup,
  savedBotSummary,
  qrSvgModel,
  telegramWebhookBlocked,
  wizardSteps,
} from './channelsSettings.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const TELEGRAM = {
  platform: 'telegram',
  configured: true,
  enabled: true,
  state: 'connected',
  display_name: '@ace_bot',
  credential_hint: '****wxyz',
  extra: { username: 'ace_bot', privacy_mode: true },
  owner: 'user:42',
  contacts: [{ principal: 'user:42', kind: 'user', name: '我', owner: true }],
  pending: [{ id: 'r1', kind: 'user', name: 'Alice', principal: 'user:7', label: 'Telegram 私聊', expires_in_s: 61 }],
  bindings: [{
    key: '["telegram","1","private","42"]',
    label: 'Telegram 私聊',
    session_id: 's-1',
    no_workspace: false,
    cwd: 'C:/work/项目A',
    sent: 3,
    failed: 1,
    dropped: 0,
  }],
};

// 场景:接口返回只有 Telegram 一个平台,QQ 缺失;字段是 snake_case。
// 期望:两个平台都有默认快照;字段转成 camelCase,数组项缺 id 的被丢弃。
run('normalizeChannelsSnapshot 补齐缺失平台并规整字段', () => {
  const state = normalizeChannelsSnapshot({
    platforms: [{ ...TELEGRAM, pending: [...TELEGRAM.pending, { name: '缺 id' }] }],
    binds: { qq: { platform: 'qq', phase: 'waiting', qr_url: 'https://q.qq.com/x', refreshes: 2 } },
  });
  assert.equal(state.platforms.qq.state, 'unconfigured');
  assert.equal(state.platforms.qq.configured, false);
  assert.equal(state.platforms.telegram.displayName, '@ace_bot');
  assert.equal(state.platforms.telegram.pending.length, 1);
  assert.equal(state.platforms.telegram.bindings[0].sessionId, 's-1');
  assert.equal(state.binds.qq.qrUrl, 'https://q.qq.com/x');
  assert.equal(state.binds.qq.refreshes, 2);
  assert.equal(state.binds.weixin.phase, 'idle');
  assert.equal(state.platforms.discord.state, 'unconfigured');
});

// 场景:WS 推来 channels_state(替换单个平台)、channels_bind(扫码进度)与无关事件。
// 期望:只改对应部分;无关事件返回同一个对象,组件可跳过重渲染。
run('applyChannelsEvent 只合并消息通道事件', () => {
  const state = normalizeChannelsSnapshot({ platforms: [TELEGRAM] });
  const next = applyChannelsEvent(state, {
    type: 'channels_state',
    payload: { ...TELEGRAM, state: 'retrying', detail: '网络中断' },
  });
  assert.equal(next.platforms.telegram.state, 'retrying');
  assert.equal(next.platforms.qq, state.platforms.qq);
  const bind = applyChannelsEvent(next, { type: 'channels_bind', payload: { platform: 'qq', phase: 'failed', error: '门户不可用' } });
  assert.equal(bind.binds.qq.phase, 'failed');
  const weixin = applyChannelsEvent(bind, { type: 'channels_bind', payload: { platform: 'weixin', phase: 'waiting', qr_url: 'https://liteapp.weixin.qq.com/q/x' } });
  assert.equal(weixin.binds.weixin.qrUrl, 'https://liteapp.weixin.qq.com/q/x');
  assert.equal(weixin.binds.qq, bind.binds.qq);
  assert.equal(applyChannelsEvent(weixin, { type: 'channels_bind', payload: { platform: 'telegram', phase: 'waiting' } }), weixin);
  assert.equal(applyChannelsEvent(bind, { type: 'token', payload: {} }), bind);
  assert.equal(applyChannelsEvent(bind, { type: 'channels_state', payload: { platform: 'whatsapp' } }), bind);
});

// 场景:设置页的平台卡片。
// 期望:七个平台按国内(QQ、微信、飞书、钉钉)再海外(Telegram、Discord、LINE)排列;图标统一是
// 单色聊天图标(用户拍板:不用彩色品牌 logo);向导都是三步,第一步按平台区分。
run('channelCards 与向导步骤', () => {
  assert.deepEqual(channelCards().map((card) => card.platform),
    ['qq', 'weixin', 'feishu', 'dingtalk', 'telegram', 'discord', 'line']);
  assert.ok(channelCards().every((card) => card.icon === 'chat'));
  assert.deepEqual(wizardSteps('qq'), ['扫码创建机器人', '连接', '绑定机主']);
  assert.deepEqual(wizardSteps('weixin'), ['扫码登录', '连接', '完成']);
  assert.deepEqual(wizardSteps('telegram'), ['创建机器人', '连接', '绑定机主']);
  assert.deepEqual(wizardSteps('line'), ['创建频道', '连接', '绑定机主']);
});

// 场景:各平台的接入方式。
// 期望:QQ、微信扫码(只有 QQ 能改手填);飞书、钉钉、Discord、LINE 填凭据并用 6 位绑定码定机主;
// Telegram 用一次性链接;飞书连上后还要回后台保存长连接订阅并发布版本,所以有连接后清单。
run('platformSetup 区分接入方式与机主绑定', () => {
  assert.equal(platformSetup('qq').credential.kind, 'scan');
  assert.equal(platformSetup('qq').credential.fields.length, 2);
  assert.equal(platformSetup('weixin').credential.kind, 'scan');
  assert.equal(platformSetup('weixin').credential.fields.length, 0);
  assert.equal(platformSetup('weixin').owner, 'scanner');
  for (const platform of ['feishu', 'dingtalk', 'discord', 'line']) {
    assert.equal(platformSetup(platform).credential.kind, 'form', platform);
    assert.equal(platformSetup(platform).owner, 'code', platform);
  }
  assert.equal(platformSetup('telegram').owner, 'link');
  assert.ok(platformSetup('feishu').connectChecklist.some((item) => item.includes('使用长连接接收事件')));
  assert.deepEqual(credentialFields('dingtalk').map((field) => field.key), ['client_id', 'client_secret']);
  assert.deepEqual(credentialFields('line').map((field) => field.key), ['channel_id', 'channel_secret', 'public_url']);
});

// 场景:Discord 校验通过后拿到 application id,向导要给出邀请链接;已保存机器人的摘要。
// 期望:邀请链接带 bot 作用域与发消息所需权限;没有 id 时不给链接;摘要只用非密钥字段与尾号。
run('discordInviteUrl 与已保存机器人摘要', () => {
  assert.equal(discordInviteUrl('123'), 'https://discord.com/oauth2/authorize?client_id=123&scope=bot&permissions=274878008320');
  assert.equal(discordInviteUrl(''), '');
  assert.deepEqual(savedBotSummary('feishu', { credentialsPublic: { app_id: 'cli_1', app_secret: '****abcd' } }),
    { title: 'App ID cli_1', desc: 'App Secret ****abcd' });
  assert.deepEqual(savedBotSummary('discord', { displayName: 'AceBot', credentialHint: '****wxyz', credentialsPublic: {} }),
    { title: 'AceBot', desc: 'Token ****wxyz' });
});

// 场景:卡片在各种状态下的按钮与状态行。
// 期望:没开时按钮是「连接」且不显示状态;开着时按钮是「取消连接」;已连接显示机器人名;
// 失败只说“详情见日志”并带「处理」入口(原因写进 daemon 日志,不在页面展开);webhook 单独提示;
// 数据文件异常时「连接」不可点;请求进行中按钮一律禁用。
run('channelCardView 按钮与简要状态', () => {
  const state = normalizeChannelsSnapshot({ platforms: [TELEGRAM] }).platforms.telegram;
  assert.deepEqual(channelCardView(state), {
    button: { action: 'disconnect', label: '取消连接', disabled: false },
    status: { tone: 'ok', text: '已连接 · @ace_bot' },
  });
  const off = { ...state, enabled: false, state: 'disabled' };
  assert.deepEqual(channelCardView(off), { button: { action: 'connect', label: '连接', disabled: false }, status: null });
  const failed = channelCardView({ ...state, state: 'failed', retryStopped: true, detail: 'token 无效' });
  assert.deepEqual(failed.status, { tone: 'danger', text: '连接失败,详情见日志', action: 'fix' });
  assert.equal(failed.status.text.includes('token'), false);
  assert.equal(channelCardView({ ...state, state: 'failed', extra: { webhook: true } }).status.text,
    '机器人设置了 webhook,需要处理');
  assert.equal(channelCardView({ ...state, state: 'retrying', detail: '网络中断' }).status.text, '连接中断,正在自动重试');
  assert.equal(channelCardView({ ...state, state: 'standby', hostedByPid: 4321 }).status.text, '已由另一个 ACECode 连接');
  assert.equal(channelCardView({ ...off, state: 'error' }).button.disabled, true);
  assert.equal(channelCardView(off, true).button.disabled, true);
  assert.equal(channelCardView(state, true).button.disabled, true);
});

// 场景:点「连接」时平台的配置程度不同。
// 期望:没有凭据或还没有机主 -> 打开向导(没有凭据从第 1 步,有凭据从第 2 步);
// 凭据与机主都齐 -> 直接连接,不弹向导。
run('connectNeedsWizard 与 initialWizardStep', () => {
  const blank = normalizeChannelsSnapshot(null).platforms.telegram;
  const configured = { ...blank, configured: true };
  const ready = { ...configured, owner: 'user:42' };
  assert.equal(connectNeedsWizard(blank), true);
  assert.equal(connectNeedsWizard(configured), true);
  assert.equal(connectNeedsWizard(ready), false);
  assert.equal(initialWizardStep(blank), 0);
  assert.equal(initialWizardStep(configured), 1);
});

// 场景:LINE 连接停在需要用户动手的几步:没装 cloudflared、控制台没开 Use webhook、
// 自己的公网地址还没转发到本机回调端口;以及普通的重试。
// 期望:前三种给出具体该做什么(含端口号);普通重试仍是通用文案;其它平台不受这些字段影响。
run('LINE 连接步骤的可操作提示', () => {
  const base = { ...normalizeChannelsSnapshot(null).platforms.line, configured: true, enabled: true };
  const missing = connectStepView({ ...base, state: 'failed', extra: { cloudflared_missing: true } });
  assert.equal(missing.phase, 'failed');
  assert.ok(missing.text.includes('winget install --id Cloudflare.cloudflared'));
  const webhookOff = connectStepView({ ...base, state: 'retrying', extra: { webhook_active: false } });
  assert.equal(webhookOff.tone, 'warn');
  assert.ok(webhookOff.text.includes('Use webhook'));
  const custom = connectStepView({ ...base, state: 'retrying', extra: { tunnel: 'custom', listen_port: 41234 } });
  assert.ok(custom.text.includes('http://127.0.0.1:41234'));
  const plain = connectStepView({ ...base, state: 'retrying', extra: { tunnel: 'cloudflare' } });
  assert.equal(plain.text, '连接中断,正在自动重试…');
  const telegram = { ...normalizeChannelsSnapshot(null).platforms.telegram, configured: true, enabled: true };
  assert.equal(connectStepView({ ...telegram, state: 'retrying', extra: { webhook_active: false } }).text,
    '连接中断,正在自动重试…');
});

// 场景:微信登录在腾讯侧失效,传输层报失败并停止重试;另有一次普通的微信连接失败。
// 期望:登录失效时点「连接」打开向导并从扫码(第 1 步)开始;普通失败仍直接重连;
// 其他平台停止重试不受影响。
run('微信登录失效时连接回到扫码', () => {
  const weixin = normalizeChannelsSnapshot(null).platforms.weixin;
  const ready = { ...weixin, configured: true, owner: 'user:o@im.wechat' };
  const expired = { ...ready, state: 'failed', retryStopped: true };
  assert.equal(connectNeedsWizard(expired), true);
  assert.equal(initialWizardStep(expired), 0);
  assert.equal(connectNeedsWizard({ ...ready, state: 'failed', retryStopped: false }), false);
  const telegram = normalizeChannelsSnapshot(null).platforms.telegram;
  const stopped = { ...telegram, configured: true, owner: 'user:42', state: 'failed', retryStopped: true };
  assert.equal(connectNeedsWizard(stopped), false);
});

// 场景:向导第 2 步(连接)期间平台状态的变化。
// 期望:还没打开 -> 正在打开;连接中 / 重试中 -> 继续等;已连接 -> 可进入下一步;
// webhook 阻塞 -> 说明后果并允许移除;其他失败 -> 只说原因已写入日志,可重试;待命 -> 说明会自动接管。
run('connectStepView 映射连接阶段', () => {
  const base = { ...normalizeChannelsSnapshot(null).platforms.telegram, configured: true };
  assert.equal(connectStepView(base).phase, 'idle');
  assert.equal(connectStepView({ ...base, enabled: true, state: 'connecting' }).phase, 'connecting');
  assert.equal(connectStepView({ ...base, enabled: true, state: 'retrying' }).phase, 'connecting');
  assert.deepEqual(connectStepView({ ...base, enabled: true, state: 'connected', displayName: '@ace_bot' }),
    { phase: 'connected', tone: 'ok', text: '已连接 @ace_bot' });
  const webhook = connectStepView({ ...base, enabled: true, state: 'failed', extra: { webhook: true } });
  assert.equal(webhook.phase, 'webhook');
  assert.match(webhook.text, /原来接收 webhook 的服务会停止收到消息/);
  const failed = connectStepView({ ...base, enabled: true, state: 'failed', detail: 'token 无效' });
  assert.equal(failed.phase, 'failed');
  assert.match(failed.text, /原因已写入日志/);
  assert.equal(connectStepView({ ...base, enabled: true, state: 'standby' }).phase, 'standby');
});

// 场景:向导第 3 步(绑定机主)。
// 期望:已有机主 -> 完成并显示机主名;Telegram 没有机主 -> 一次性链接;QQ 扫码后的 10 分钟窗口内 ->
// 提示去私聊;窗口已过 -> 提示私聊后批准请求(批准第一个私聊请求即成为机主)。
run('ownerStepView 按平台给出绑定方式', () => {
  const telegram = normalizeChannelsSnapshot({ platforms: [TELEGRAM] }).platforms.telegram;
  assert.deepEqual(ownerStepView(telegram, 'telegram'), { done: true, mode: 'done', text: '机主:我' });
  assert.equal(ownerStepView({ ...telegram, owner: '' }, 'telegram').mode, 'link');
  const qq = { ...normalizeChannelsSnapshot(null).platforms.qq, configured: true };
  assert.equal(ownerStepView({ ...qq, ownerWindow: true }, 'qq').mode, 'window');
  assert.equal(ownerStepView(qq, 'qq').mode, 'request');
  assert.equal(ownerStepView({ ...qq, owner: 'user:9' }, 'qq').text, '机主:用户 9');
  const discord = { ...normalizeChannelsSnapshot(null).platforms.discord, configured: true };
  assert.equal(ownerStepView(discord, 'discord').mode, 'code');
  assert.match(ownerStepView(discord, 'discord').text, /6 位数字/);
});

// 场景:扫码流程各阶段。
// 期望:进行中阶段 active=true;等待阶段有二维码才显示;过期刷新后提示扫新码;
// 完成时区分扫码人是否已成为机主;失败附原因并提示可改为手动填写。
run('bindView 映射扫码阶段', () => {
  assert.equal(bindView({ phase: 'starting' }).active, true);
  const waiting = bindView({ phase: 'waiting', qrUrl: 'u', refreshes: 0 });
  assert.equal(waiting.showQr, true);
  assert.equal(waiting.message, '用手机 QQ 扫一扫,按提示创建并授权机器人');
  assert.match(bindView({ phase: 'waiting', qrUrl: 'u', refreshes: 1 }).message, /自动换新/);
  assert.equal(bindView({ phase: 'completed', ownerBound: true }).message, '扫码成功,你已是机主');
  assert.equal(bindView({ phase: 'completed', ownerBound: false }).message, '扫码成功');
  assert.match(bindView({ phase: 'failed', error: '' }).message, /手动填写/);
  assert.equal(bindView({ phase: 'failed', error: '' }, 'weixin').message, '获取二维码失败,请重试');
  assert.equal(bindView({ phase: 'waiting', qrUrl: 'u' }, 'weixin').message, '用微信扫一扫,并在手机上确认');
  assert.equal(bindView({ phase: 'failed', error: '门户不可用' }).message, '门户不可用');
  assert.equal(bindView({ phase: 'idle' }).active, false);
});

// 场景:授权名单与待批准请求的展示行。
// 期望:身份转成中文标签;机主标“机主”;请求显示过期分钟数。
run('列表行规整', () => {
  const state = normalizeChannelsSnapshot({ platforms: [TELEGRAM] }).platforms.telegram;
  assert.equal(principalLabel('member:G1:M1'), '群 G1 的成员 M1');
  assert.equal(principalLabel('group:-100'), '群 -100');
  assert.equal(principalLabel('user:7'), '用户 7');
  assert.equal(contactRows(state)[0].kindLabel, '机主');
  assert.equal(pendingRows(state)[0].subtitle, '私聊 · Telegram 私聊 · 2 分钟后过期');
});

// 场景:Telegram 机器人被配置了 webhook,或者没有。
// 期望:只有 extra.webhook === true 才判定为被阻塞。
run('telegramWebhookBlocked', () => {
  assert.equal(telegramWebhookBlocked({ extra: { webhook: true } }), true);
  assert.equal(telegramWebhookBlocked({ extra: {} }), false);
});

// 场景:有人把细节加回设置页(2026-10-05 用户验收反馈:设置界面太复杂,细节打印到日志)。
// 期望:页面组件不渲染原因 / 隐私模式 / 暂存数 / 绑定计数这些细节;卡片两列;不用开关,用连接按钮。
run('消息通道设置页保持简洁,细节只进日志', () => {
  const page = readFileSync(new URL('../components/ChannelsSettings.jsx', import.meta.url), 'utf8');
  assert.match(page, /grid grid-cols-2/);
  assert.doesNotMatch(page, /platform\.detail|privacy_mode|\.held\b|bindingRows|binding\.stats/);
  assert.doesNotMatch(page, /<Toggle\b/);
});

// 场景:手动填写凭据:两端带空白、只改一个字段、全空。
// 期望:去空白后提交,空字段省略(沿用已保存的值),全空返回 null 不发请求。
run('credentialsPayload 与占位文案', () => {
  assert.deepEqual(credentialsPayload('qq', { app_id: ' 1020 ', app_secret: '' }), { app_id: '1020' });
  assert.deepEqual(credentialsPayload('telegram', { token: ' 1:abc ' }), { token: '1:abc' });
  assert.equal(credentialsPayload('telegram', { token: '  ' }), null);
  // 飞书的版本选择默认飞书;只选了版本、没填任何凭据不算有内容。
  assert.deepEqual(credentialsPayload('feishu', { app_id: 'cli_1', app_secret: 's' }),
    { app_id: 'cli_1', app_secret: 's', domain: 'feishu' });
  assert.equal(credentialsPayload('feishu', {}), null);
  // LINE 公网地址留空表示清除(改回自动隧道),所以即使别的都没填也要提交。
  assert.deepEqual(credentialsPayload('line', { public_url: ' ' }), { public_url: '' });
  assert.deepEqual(credentialsPayload('line', { channel_id: '1', channel_secret: 's', public_url: 'https://x.example' }),
    { channel_id: '1', channel_secret: 's', public_url: 'https://x.example' });
  const token = credentialFields('telegram')[0];
  assert.equal(credentialPlaceholder({ credentialsPublic: { token: '****wxyz' } }, token), '已保存 ****wxyz,留空则不修改');
  assert.equal(credentialPlaceholder({ credentialsPublic: {} }, token), 'Bot Token');
  const appId = credentialFields('feishu')[0];
  assert.equal(credentialPlaceholder({ credentialsPublic: { app_id: 'cli_9' } }, appId), 'cli_9(留空则不修改)');
});

// 场景:把扫码链接画成二维码。
// 期望:边长 = 模块数 + 两侧各 4 格静区;路径里的深色格数与库给出的深色模块数一致;空内容返回 null。
run('qrSvgModel 生成带静区的二维码路径', () => {
  const content = 'https://q.qq.com/qqbot/openclaw/connect.html?task_id=abc&source=acecode&_wv=2';
  const model = qrSvgModel(content);
  assert.equal(model.size, model.modules + 8);
  const code = qrcode(0, 'M');
  code.addData(content);
  code.make();
  let dark = 0;
  for (let r = 0; r < code.getModuleCount(); r += 1)
    for (let c = 0; c < code.getModuleCount(); c += 1) if (code.isDark(r, c)) dark += 1;
  const covered = [...model.path.matchAll(/h(\d+)v1/g)].reduce((sum, match) => sum + Number(match[1]), 0);
  assert.equal(covered, dark);
  assert.equal(qrSvgModel(''), null);
});

// 场景:会话列表行带 channel_bound,或只有 /rc 绑定,或平台名未知。
// 期望:只认 QQ / Telegram;悬停提示写明平台;/rc 绑定不影响这个判定。
run('channelBoundPlatform 只认消息通道平台', () => {
  assert.equal(channelBoundPlatform({ channel_bound: { platform: 'qq' } }), 'qq');
  assert.equal(channelBoundPlatform({ channelBound: { platform: 'telegram' } }), 'telegram');
  assert.equal(channelBoundPlatform({ remote_control_bound: true }), '');
  assert.equal(channelBoundPlatform({ channel_bound: { platform: 'whatsapp' } }), '');
  assert.equal(channelBoundLabel('qq'), '已绑定到 QQ');
  assert.equal(channelBoundLabel(''), '');
});

// 场景:绑定从会话 A 移到会话 B,或只是发送计数变化。
// 期望:签名只在绑定的会话集合变化时改变,侧栏据此决定是否立即刷新。
run('绑定签名只随会话集合变化', () => {
  const state = normalizeChannelsSnapshot({ platforms: [TELEGRAM] });
  const moved = applyChannelsEvent(state, {
    type: 'channels_state',
    payload: { ...TELEGRAM, bindings: [{ ...TELEGRAM.bindings[0], session_id: 's-2' }] },
  });
  const counted = applyChannelsEvent(state, {
    type: 'channels_state',
    payload: { ...TELEGRAM, bindings: [{ ...TELEGRAM.bindings[0], sent: 9 }] },
  });
  assert.notEqual(channelBindingSignature(moved), channelBindingSignature(state));
  assert.equal(channelBindingSignature(counted), channelBindingSignature(state));
  assert.equal(channelStateBindingSignature(TELEGRAM), 's-1');
  assert.equal(channelStateBindingSignature({ platform: 'whatsapp' }), null);
});

// 场景:接口返回带中文 message 的 400、远程客户端的 403、网络异常。
// 期望:分别展示后端文案、本机设置提示、异常自身的消息。
run('channelErrorText 选择展示文案', () => {
  assert.equal(channelErrorText({ code: 'CHANNEL_ERROR', body: { error: 'CHANNEL_ERROR', message: 'Token 无效' } }), 'Token 无效');
  assert.equal(channelErrorText({ code: 'LOCAL_ONLY', body: { error: 'LOCAL_ONLY' } }), '消息通道只能在运行 ACECode 的这台电脑上设置');
  assert.equal(channelErrorText(new Error('Failed to fetch')), 'Failed to fetch');
  assert.equal(channelErrorText(null), '操作失败');
});

// 场景:侧栏渲染被消息通道绑定的会话。
// 期望:与 /rc 共用电脑图标并带平台提示;/rc 的换绑动画类名仍只由 remoteControlBound 决定。
run('Sidebar 用 channelBoundPlatform 显示通道绑定图标', () => {
  const sidebar = readFileSync(new URL('../components/Sidebar.jsx', import.meta.url), 'utf8');
  assert.match(sidebar, /channelBoundPlatform\(s\)/);
  assert.match(sidebar, /data-channel-session-icon=\{channelPlatform\}/);
  assert.match(sidebar, /remoteControlBound && 'is-remote-control-bound'/);
  assert.doesNotMatch(sidebar, /channelPlatform && 'is-remote-control-bound'/);
});
