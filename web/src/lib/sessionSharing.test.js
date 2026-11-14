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
async function component(name, bindings = {}) {
  const code = source(name).replace(/^import .*;\r?\n/gm, '').replace(/^export /gm, '');
  const compiled = await transformWithEsbuild(code, `${name}.jsx`, {
    loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
  });
  return vm.runInNewContext(`${compiled.code}; ${name};`, { React, ...React, ...bindings });
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

// 场景：生成可交互链接的请求尚未返回，不能切成只读后复制旧权限的结果。
const shareSlots = [];
let shareIndex = 0;
let finishCreate;
const shareApi = {
  createShare: () => new Promise((resolve) => { finishCreate = resolve; }),
  listShares: async () => ({ shares: [] }), listPeers: async () => ({ peers: [] }),
};
const ShareSessionDialog = await component('ShareSessionDialog', {
  Modal, VsIcon: () => null, inviteErrorMessage, Event,
  window: { dispatchEvent() {} }, useEffect() {},
  useState(initial) {
    const slot = shareIndex++;
    if (!(slot in shareSlots)) shareSlots[slot] = initial;
    return [shareSlots[slot], (value) => { shareSlots[slot] = typeof value === 'function' ? value(shareSlots[slot]) : value; }];
  },
});
const renderShare = () => { shareIndex = 0; return ShareSessionDialog({ api: shareApi, sessionId: 's', title: 'Title' }); };
let shareTree = renderShare();
const pendingCreate = find(shareTree, (node) => node.type === 'button' && node.props['data-ace-dialog-primary']).props.onClick();
shareTree = renderShare();
assert.equal(find(shareTree, (node) => node.props?.role === 'radio').props.disabled, true);
assert.equal(find(shareTree, (node) => node.type === 'select' && node.props.value === 'conversation').props.disabled, true);
assert.equal(find(shareTree, (node) => node.type === 'select' && node.props.value === 86400).props.disabled, true);
finishCreate({ link: 'generated-interactive-link' });
await pendingCreate;
shareTree = renderShare();
assert.equal(find(shareTree, (node) => node.props?.role === 'radio').props.disabled, false);
assert.equal(find(shareTree, (node) => node.type === 'input').props.value, 'generated-interactive-link');
console.log('[pass] session sharing permissions, text escaping, confirmation and cancellation');
