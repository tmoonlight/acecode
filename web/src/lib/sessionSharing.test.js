import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { transformWithEsbuild } from 'vite';
import { sessionTitleActionStates } from './sessionTitleActions.js';
import { decodeInviteLink, inviteErrorMessage } from './inviteLink.js';
import { INVITE_LINK_EVENT, SHARE_SESSION_EVENT } from './sharingEvents.js';

const source = (name) => readFileSync(new URL(`../components/${name}.jsx`, import.meta.url), 'utf8');
async function component(name, bindings = {}, exported = name) {
  const code = source(name).replace(/^import\s[\s\S]*?;\r?\n/gm, '').replace(/^export /gm, '');
  const compiled = await transformWithEsbuild(code, `${name}.jsx`, {
    loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
  });
  return vm.runInNewContext(`${compiled.code}; ${exported};`, { React, ...React, ...bindings });
}
const Modal = ({ children }) => React.createElement('section', {}, children);
const ShareDialogHeader = ({ title }) => React.createElement('h2', {}, title);
const RemoteSessionView = await component('RemoteSessionView', { Modal, ShareDialogHeader });

// 场景：只读链接进入远程视图，根本不挂载聊天与审批入口；可交互链接才显示提问。
for (const allow_questions of [false, true]) {
  const html = renderToStaticMarkup(React.createElement(RemoteSessionView, {
    api: {}, peer: { id: 'saved', owner: '<script>owner</script>', title: 'Title', allow_questions }, onClose() {},
  }));
  assert.equal(html.includes('data-guest-question'), allow_questions);
  assert.equal(html.includes('<textarea'), allow_questions);
  assert.ok(html.includes('&lt;script&gt;owner&lt;/script&gt;'));
  assert.ok(!html.includes('<script>'));
}
assert.doesNotMatch(source('RemoteSessionView'), /api\.(?:send|approve|resume|getSession|updateSession)\b|PermissionPrompt|<InputBar/);
assert.match(source('RemoteSessionView'), /externalContent/);
const titleActions = source('SessionTitleActions');
const shareIconIndex = titleActions.indexOf('<VsIcon name="share" size={16}');
assert.ok(shareIconIndex >= 0 && shareIconIndex < titleActions.indexOf('name="properties"'));
assert.doesNotMatch(titleActions, /<svg\b|ShareIcon/);
// 场景：侧栏会话行里的已分享标记。期望：使用 Antenna 发射天线图形并按 18 px 渲染，与同一行的远程控制、置顶图标同尺寸。
// 回归：曾用 12 px 雷达图形，两圈弧线加扫描线和两个点在 1x 屏幕上糊成一团，且明显小于旁边的图标。
const sharedBadge = source('SharedSessionBadge');
assert.match(sharedBadge, /<VsIcon name="antenna" size=\{18\}/);
assert.doesNotMatch(sharedBadge, /radar/i);
assert.equal(sessionTitleActionStates({ sessionId: 's' }).share.enabled, true);
assert.equal(sessionTitleActionStates({ sessionId: 's', readOnly: true }).share.enabled, false);

// 场景：邀请到达后先展示纯文本确认；取消不联网，确认才向本地 daemon 发连接请求。
const slots = [];
const listeners = new Map();
let index = 0;
const pendingEffects = [];
const bindings = {
  Modal, ShareDialogHeader, ShareSessionDialog: () => null, RemoteSessionView: () => null,
  decodeInviteLink, inviteErrorMessage, INVITE_LINK_EVENT, SHARE_SESSION_EVENT,
  window: { addEventListener: (name, fn) => listeners.set(name, fn), removeEventListener: (name) => listeners.delete(name) },
  useState(initial) {
    const slot = index++;
    if (!(slot in slots)) slots[slot] = initial;
    return [slots[slot], (value) => { slots[slot] = typeof value === 'function' ? value(slots[slot]) : value; }];
  },
  useRef(initial) { const slot = index++; return slots[slot] ??= { current: initial }; },
  useEffect(effect) { const slot = index++; if (!(slot in slots)) { slots[slot] = true; pendingEffects.push(effect); } },
};
let connections = 0;
let cancelled = 0;
bindings.api = { connectPeer: async () => { connections++; return { id: 'saved' }; } };
const SharingHost = await component('SharingHost', bindings);
const render = () => { index = 0; const tree = SharingHost(); pendingEffects.splice(0).forEach((effect) => effect()); return tree; };
const find = (node, predicate) => {
  if (!node || typeof node !== 'object') return null;
  if (Array.isArray(node)) return node.map((child) => find(child, predicate)).find(Boolean);
  if (predicate(node)) return node;
  return find(node.props?.children, predicate);
};
const fixture = JSON.parse(readFileSync(new URL('../../../tests/fixtures/invite_links/valid.json', import.meta.url), 'utf8'));
// 固定向量的过期时刻远于当前时间，前端仍执行完整校验。
render();
listeners.get(INVITE_LINK_EVENT)({ detail: { link: fixture.text, onCancel: () => cancelled++ } });
let tree = render();
assert.equal(connections, 0);
find(tree, (node) => node.type === 'button' && node.props.children === '取消').props.onClick();
assert.equal(cancelled, 1);
assert.equal(connections, 0);
listeners.get(INVITE_LINK_EVENT)({ detail: { link: fixture.text } });
tree = render();
const connect = find(tree, (node) => node.type === 'button' && node.props['data-ace-dialog-primary']);
assert.equal(connect.props.disabled, false);
await connect.props.onClick();
assert.equal(connections, 1);

// 场景：默认只读，开关决定新链接是否可交互；内容和两天有效期固定，生成期间不能切换权限。
const shareSlots = [];
let shareIndex = 0;
let finishCreate;
const createRequests = [];
const shareApi = {
  createShare: (sessionId, options) => { createRequests.push({ sessionId, options }); return new Promise((resolve) => { finishCreate = resolve; }); },
  listShares: async () => ({ shares: [] }), listPeers: async () => ({ peers: [] }),
};
const Toggle = await component('Modal', { clsx: (...values) => values.filter(Boolean).join(' ') }, 'Toggle');
const ShareSessionDialog = await component('ShareSessionDialog', {
  Modal, Toggle, VsIcon: ({ name }) => React.createElement('span', { 'data-icon-name': name }), inviteErrorMessage, Event,
  window: { dispatchEvent() {} }, useEffect() {},
  useState(initial) {
    const slot = shareIndex++;
    if (!(slot in shareSlots)) shareSlots[slot] = initial;
    return [shareSlots[slot], (value) => { shareSlots[slot] = typeof value === 'function' ? value(shareSlots[slot]) : value; }];
  },
});
const renderShare = () => { shareIndex = 0; return ShareSessionDialog({ api: shareApi, sessionId: 's', title: 'Title' }); };
let shareTree = renderShare();
const initialShareHtml = renderToStaticMarkup(shareTree);
assert.match(initialShareHtml, /role="switch" aria-checked="false" aria-label="可交互"/);
assert.match(initialShareHtml, /已存在的链接/);
assert.doesNotMatch(initialShareHtml, /<select|role="radio"|可见内容|有效期|可信局域网|token额度|warningCircle/);
for (const interactive of [false, true]) {
  if (interactive) {
    find(shareTree, (node) => node.type === Toggle).props.onChange(true);
    shareTree = renderShare();
    const html = renderToStaticMarkup(shareTree);
    assert.match(html, /打开可交互会话会消耗共享用户的token额度。/);
    assert.match(html, /data-icon-name="warningCircle"/);
    assert.equal(find(shareTree, (node) => node.type === 'input'), undefined);
  }
  const pendingCreate = find(shareTree, (node) => node.type === 'button' && node.props['data-ace-dialog-primary']).props.onClick();
  assert.equal(JSON.stringify(createRequests.at(-1)), JSON.stringify({ sessionId: 's', options: {
    mode: interactive ? 'interactive' : 'read_only', visibility: 'tools', expires_in: 172800,
  } }));
  shareTree = renderShare();
  assert.equal(find(shareTree, (node) => node.type === Toggle).props.disabled, true);
  const link = `generated-${interactive ? 'interactive' : 'readonly'}-link`;
  finishCreate({ link });
  await pendingCreate;
  shareTree = renderShare();
  assert.equal(find(shareTree, (node) => node.type === Toggle).props.disabled, false);
  assert.equal(find(shareTree, (node) => node.type === 'input').props.value, link);
}
find(shareTree, (node) => node.type === Toggle).props.onChange(false);
shareTree = renderShare();
assert.doesNotMatch(renderToStaticMarkup(shareTree), /token额度|warningCircle|generated-interactive-link/);
find(shareTree, (node) => node.type === 'button' && node.props.children === '已存在的链接').props.onClick();
assert.match(renderToStaticMarkup(renderShare()), /<h2[^>]*>已存在的链接<\/h2>/);
console.log('[pass] session sharing permissions, text escaping, confirmation and cancellation');
