import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import * as composerContent from './composerContent.js';
import * as composerImagePresentation from './composerImagePresentation.js';
import * as messageAttachments from './messageAttachments.js';
import * as desktopContext from './desktopContextMenu.js';
import { resolveLeadingSlashCommand } from './slashCommands.js';
import { extractSessionReferences, formatSessionReferenceToken } from './sessionReference.js';
import * as pastedText from './pastedText.js';
import * as userMessagePreview from './userMessagePreview.js';
import { clsx } from './format.js';

function run(name, fn) { fn(); console.log(`[pass] ${name}`); }
async function compileComponentSource(file) {
  const source = readFileSync(new URL(`../components/${file}`, import.meta.url), 'utf8');
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const body = ast.program.body.filter((node) => node.type !== 'ImportDeclaration').map((node) => node.declaration || node);
  const transformed = await transformWithEsbuild(body.map((node) => source.slice(node.start, node.end)).join('\n'), file, {
    loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
  });
  return transformed.code;
}
const messageCode = await compileComponentSource('Message.jsx');
const dialogCode = await compileComponentSource('PastedTextDialog.jsx');
const commands = [{ name: 'review', kind: 'skill', description: 'Review source', path: '/skills/review/SKILL.md' }];
// 卡片 / 对话框用桩:只把关键 props 暴露成 data 属性,便于断言渲染位置与来源。
const PastedTextCardStub = ({ title, sizeBytes, status }) => React.createElement('span', {
  'data-card': title, 'data-card-size': sizeBytes, 'data-card-status': status,
});
const PastedTextDialogStub = () => null;
const contextLoader = () => Promise.resolve('');
const LoaderContext = React.createContext(contextLoader);
function compile(overrides = {}) {
  return vm.runInNewContext(`${messageCode}; ({ Message, OrderedUserMessageBody });`, {
    React, ...React, ...composerContent, ...composerImagePresentation, ...messageAttachments, ...desktopContext,
    ...pastedText, ...userMessagePreview, clsx,
    useTranslation() {}, useSlashCommands: () => ({ commands }), resolveLeadingSlashCommand, extractSessionReferences,
    VsIcon: () => null, CommandGlyph: () => null, FileTypeIcon: () => null,
    ImageLightbox: () => null,
    AttachmentTextLoaderContext: LoaderContext,
    PastedTextCard: PastedTextCardStub,
    PastedTextDialog: PastedTextDialogStub,
    AttachmentStrip: ({ contentParts = [] }) => React.createElement('aside', {}, contentParts.map((part, index) => (
      React.createElement('span', { key: index }, part.attachment?.name || part.context?.label || '')
    ))),
    ...overrides,
  });
}
// 直接把组件当函数调用时用的极简 hook:useState 按调用顺序存槽位,reset() 后重放。
function hookHarness() {
  const slots = [];
  let index = 0;
  return {
    reset() { index = 0; },
    useState(initial) {
      const slot = index++;
      if (!(slot in slots)) slots[slot] = typeof initial === 'function' ? initial() : initial;
      return [slots[slot], (value) => { slots[slot] = typeof value === 'function' ? value(slots[slot]) : value; }];
    },
  };
}
const { Message } = compile();
// 与输入框产出的文档同形:图片部件排在最前(输入框把图片放在上方缩略图条),
// 普通文件附件可以夹在文字中间。
const imagePart = { type: 'attachment', key: 'local-a', id: 'a', name: 'diagram.png', kind: 'image' };
const skillPart = { type: 'skill', name: 'review', token: '$review', path: '/skills/review/SKILL.md' };
const filePart = { type: 'attachment', key: 'local-b', id: 'b', name: 'spec.pdf', kind: 'file', mime_type: 'application/pdf' };
const ordered = { version: 1, parts: [
  imagePart,
  { type: 'text', text: 'first ' },
  skillPart,
  { type: 'text', text: ' second ' },
  { type: 'path', path: 'src/a.cpp', token: '@src/a.cpp' },
  { type: 'text', text: ' third ' },
  filePart,
  { type: 'text', text: ' last' },
] };
const contentParts = [
  { type: 'image', attachment: { id: 'a', name: 'diagram.png', kind: 'image', mime_type: 'image/png', blob_url: '/image/blob', path: '/stored/diagram.png' } },
  { type: 'file', attachment: { id: 'b', name: 'spec.pdf', kind: 'file', mime_type: 'application/pdf', blob_url: '/file/blob', path: '/stored/spec.pdf' } },
  { type: 'selection_context', context: { label: 'selected passage' } },
];
const render = (props = {}) => renderToStaticMarkup(React.createElement(Message, { role: 'user', showFooter: false, ...props }));

// 触发场景:已发送的用户消息,composer_content 里有图片、技能、路径、普通文件与文字。
// 期望行为:技能 / 路径 / 普通文件按原顺序内联在正文里;图片在气泡上方的缩略图条里
// 出现且只出现一次,正文里没有它的文件名按钮。
// 回归:曾经所有 attachment 部件都走正文内联按钮并被从缩略图条剔除,用户消息里的图片
// 只剩一个「diagram.png」文件名,看不到缩略图。
run('sent message keeps references inline in order and shows images as thumbnails above the bubble', () => {
  const html = render({ content: 'wire text', composerContent: ordered, contentParts });
  const bubble = html.indexOf('ace-user-message-bubble');
  const positions = ['first ', '>review<', ' second ', '>src/a.cpp<', ' third ', '>spec.pdf<', ' last'].map((part) => html.indexOf(part));
  assert.ok(positions.every((position, index) => position > bubble && (index === 0 || position > positions[index - 1])));
  assert.equal((html.match(/>diagram.png</g) || []).length, 1);
  assert.ok(html.indexOf('<aside><span>diagram.png</span>') >= 0, 'image goes to the thumbnail strip');
  assert.ok(html.indexOf('<aside>') < bubble, 'thumbnail strip sits above the bubble');
  assert.doesNotMatch(html, /data-desktop-attachment-id="a"/, 'no inline file-name token for the image');
  assert.equal((html.match(/>spec.pdf</g) || []).length, 1, 'inline file is not duplicated in the strip');
  assert.match(html, /data-desktop-attachment-url="\/file\/blob"/);
  assert.match(html, /selected passage/);
  assert.doesNotMatch(html, /wire text/);
});

// 触发场景:只带附件、没有文字的用户消息。
// 期望行为:只有图片时只渲染缩略图条,不画空气泡;只有普通文件时照旧在气泡里内联。
run('persisted metadata and attachment-only prompts render through the same ordered path', () => {
  const html = render({ metadata: { composer_content: ordered }, contentParts });
  assert.match(html, /first /);
  assert.match(html, / last/);
  const imageOnly = render({ composerContent: { version: 1, parts: [imagePart] }, contentParts: [contentParts[0]] });
  assert.doesNotMatch(imageOnly, /ace-user-message-bubble/);
  assert.equal((imageOnly.match(/>diagram.png</g) || []).length, 1);
  const fileOnly = render({ composerContent: { version: 1, parts: [filePart] }, contentParts: [contentParts[1]] });
  assert.match(fileOnly, /ace-user-message-bubble/);
  assert.equal((fileOnly.match(/>spec.pdf</g) || []).length, 1);
});

run('ordered session references use readable titles before and after inline references without exposing encoded payloads', () => {
  const first = formatSessionReferenceToken({ id: 'session-a', title: 'Earlier task', workspace_hash: 'workspace-a' });
  const second = formatSessionReferenceToken({ id: 'session-b', title: '<script>Task</script>', workspace_hash: 'workspace-b' });
  const composerContent = { version: 1, parts: [
    { type: 'text', text: `Read ${first}with ` },
    skillPart,
    { type: 'text', text: ` and ${second}then ` },
    filePart,
    { type: 'text', text: ' without changing @session:%not-valid' },
  ] };
  for (const props of [{ composerContent }, { metadata: { composer_content: composerContent } }]) {
    const html = render({ ...props, contentParts });
    assert.match(html, /Read @Earlier task with /);
    assert.match(html, / and @&lt;script&gt;Task&lt;\/script&gt; then /);
    assert.doesNotMatch(html, /@session:%7B|session-a|session-b|<script>/);
    assert.match(html, />review</);
    assert.match(html, />spec.pdf</);
    assert.match(html, /without changing @session:%not-valid/);
  }
});

run('legacy and unsupported metadata retain existing slash display and attachment placement', () => {
  for (const metadata of [undefined, { composer_content: { version: 2, parts: ordered.parts } }]) {
    const html = render({ content: '/review hello', contentParts: [contentParts[0]], metadata });
    assert.match(html, />review</);
    assert.match(html, / hello/);
    assert.ok(html.indexOf('diagram.png') < html.indexOf('ace-user-message-bubble'));
  }
  const structured = render({ composerContent: composerContent.composerContentFromText('/review hello') });
  assert.match(structured, />review</);
  assert.match(structured, / hello/);
});

// 触发场景:乐观渲染的用户消息,附件还没有服务端 id,只能按 local_id(部件 key)对上。
// 期望行为:内联文件按 key 只渲染一次(不再进缩略图条);待上传的图片在缩略图条里
// 出现一次;文档里没有引用的其它资源仍留在缩略图条。
run('optimistic pending attachment is rendered once by stable key while other unreferenced resources stay visible', () => {
  const html = render({ composerContent: { version: 1, parts: [
    { type: 'attachment', key: 'pending-img', id: '', name: 'pending.png', kind: 'image' },
    { type: 'text', text: 'before ' },
    { type: 'attachment', key: 'pending-a', id: '', name: 'pending.pdf', kind: 'file' },
    { type: 'text', text: ' after' },
  ] }, contentParts: [
    { type: 'image', attachment: { local_id: 'pending-img', name: 'pending.png', preview_url: 'blob:pending-img', kind: 'image' } },
    { type: 'file', attachment: { local_id: 'pending-a', name: 'pending.pdf', kind: 'file', path: '/tmp/pending.pdf' } },
    { type: 'file', attachment: { local_id: 'extra-b', id: '', name: 'extra.txt', kind: 'file' } },
  ] });
  assert.equal((html.match(/>pending.pdf</g) || []).length, 1);
  assert.match(html, /data-desktop-attachment-id="pending-a"/);
  assert.equal((html.match(/>pending.png</g) || []).length, 1);
  assert.match(html, /<aside><span>pending.png<\/span><span>extra.txt<\/span><\/aside>/);
});

run('inline text and labels are escaped as React text', () => {
  const html = render({ composerContent: { version: 1, parts: [
    { type: 'text', text: '<script>alert(1)</script>' },
    { type: 'path', path: '<img onerror=bad>', token: '@bad' },
  ] } });
  assert.doesNotMatch(html, /<script|<img/);
  assert.match(html, /&lt;script&gt;/);
});

// 触发场景:点击正文里的路径 / 目录 / 文件附件按钮,以及桌面右键「预览」。
// 期望行为:文件与目录走各自的打开回调;图片不在正文里,它的右键预览归 AttachmentStrip,
// 正文不接(否则两处同时弹灯箱)。
run('actual inline handlers preserve file, directory, attachment and desktop preview actions', () => {
  let preview;
  let file;
  let directory;
  let desktopHandler;
  const { OrderedUserMessageBody } = compile({
    useState: () => [null, (value) => { preview = value; }],
    useMemo: (factory) => factory(), useCallback: (handler) => handler,
    useEffect: (effect) => effect(),
    useContext: (context) => context._currentValue,
    window: { addEventListener: (_event, handler) => { desktopHandler = handler; }, removeEventListener() {} },
  });
  const tree = OrderedUserMessageBody({ composerContent: { ...ordered, parts: [...ordered.parts,
    { type: 'path', path: 'src/', token: '@src/', directory: true },
  ] }, contentParts, onOpenFilePreview: (path) => { file = path; }, onLocateInFileTree: (path) => { directory = path; } });
  const buttons = tree.props.children.find(Array.isArray).filter((child) => child?.type === 'button');
  buttons.find((child) => child.props['data-file-path'] === 'src/a.cpp').props.onClick();
  assert.equal(file, 'src/a.cpp');
  buttons.find((child) => child.props['data-file-path'] === 'src/').props.onClick();
  assert.equal(directory, 'src/');
  assert.equal(buttons.some((child) => child.props['data-desktop-attachment-id'] === 'a'), false);
  buttons.find((child) => child.props['data-desktop-attachment-id'] === 'b').props.onClick();
  assert.equal(file, '/stored/spec.pdf');
  file = null;
  const detail = { action: desktopContext.DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT, target: { type: 'attachment', id: 'b' } };
  desktopHandler({ detail });
  assert.equal(detail.handled, true);
  assert.equal(file, '/stored/spec.pdf');
  const imageDetail = { action: desktopContext.DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT, target: { type: 'attachment', id: 'a' } };
  desktopHandler({ detail: imageDetail });
  assert.equal(imageDetail.handled, undefined);
  assert.equal(preview, undefined);
});

// ---- 粘贴的文本块与超长消息(第 2 条反馈 f300) ----

const pastedBody = ['first line of paste', ...Array(200).fill('SECRET-BODY-LINE')].join(String.fromCharCode(10));
const pasteMessage = { version: 1, parts: [
  { type: 'text', text: 'please review' },
  { type: 'pasted_text', key: 'paste-1', text: pastedBody },
  { type: 'attachment', key: 'paste-file', id: 'f1', name: 'pasted-text-20260924-101010.txt', kind: 'file',
    mime_type: 'text/plain', paste: { title: 'server.log head', chars: 300000, lines: 9000 } },
] };
const pasteContentParts = [
  { type: 'file', attachment: { id: 'f1', name: 'pasted-text-20260924-101010.txt', kind: 'file', mime_type: 'text/plain',
    size_bytes: 1234567, blob_url: '/api/sessions/s1/attachments/f1/blob', metadata: { origin: 'pasted_text' } } },
];

// 触发场景:用户消息带一个内联粘贴块。
// 期望行为:块渲染成卡片(标题取正文第一条非空行),气泡 HTML 里不含块的正文。
run('inline pasted block renders as a card without its body', () => {
  const html = render({ composerContent: { version: 1, parts: pasteMessage.parts.slice(0, 2) } });
  assert.match(html, /data-card="first line of paste"/);
  assert.match(html, /please review/);
  assert.doesNotMatch(html, /SECRET-BODY-LINE/);
  assert.ok(html.indexOf('data-card=') < html.indexOf('please review'), 'cards sit above the text');
});

// 触发场景:用户消息带一个文件块(attachment 部件 + paste 描述),content_parts 里有对应附件记录。
// 期望行为:渲染成带大小的卡片;不出现在 AttachmentStrip,也不落进内联附件按钮分支。
run('file pasted block renders as a card only, not in the attachment strip or inline attachment tokens', () => {
  const html = render({ composerContent: pasteMessage, contentParts: pasteContentParts });
  assert.match(html, /data-card="server.log head" data-card-size="1234567"/);
  assert.doesNotMatch(html, /<aside><span>pasted-text-/);
  assert.doesNotMatch(html, /data-desktop-attachment-id="f1"/);
  assert.equal((html.match(/pasted-text-20260924-101010\.txt/g) || []).length, 0);
});

// 触发场景:在对话记录里点开文件块卡片。
// 期望行为:只读对话框拿到的是 AttachmentTextLoaderContext 提供的 loader(远程 Web 要带
// token,不能用裸 URL),来源 URL 是附件记录的 blob_url。
run('opening a file block uses the context loader and the record blob_url', () => {
  const hooks = hookHarness();
  const providedLoader = () => Promise.resolve('file text');
  const { OrderedUserMessageBody } = compile({
    useState: hooks.useState,
    useMemo: (factory) => factory(), useCallback: (handler) => handler, useEffect() {},
    useContext: (context) => (context === LoaderContext ? providedLoader : context._currentValue),
    window: { addEventListener() {}, removeEventListener() {} },
  });
  const props = { composerContent: composerContent.normalizeComposerContent(pasteMessage), contentParts: pasteContentParts };
  const first = OrderedUserMessageBody(props);
  const cards = first.props.children[0].props.children;
  assert.equal(cards.length, 2);
  assert.equal(first.props.children.at(-1), null);
  cards.find((card) => card.props.title === 'server.log head').props.onOpen();
  hooks.reset();
  const second = OrderedUserMessageBody(props);
  const dialog = second.props.children.at(-1);
  assert.equal(dialog.type, PastedTextDialogStub);
  assert.equal(dialog.props.loader, providedLoader);
  assert.equal(dialog.props.readOnly, true);
  assert.equal(dialog.props.source.url, '/api/sessions/s1/attachments/f1/blob');
  cards.find((card) => card.props.title !== 'server.log head').props.onOpen();
  hooks.reset();
  const inline = OrderedUserMessageBody(props).props.children.at(-1);
  assert.equal(inline.props.source.text, pastedBody);
});

// 回归:f300 的 24,597,780 字符旧消息(没有 composer_content)切进会话时整段进气泡,页面卡死。
// 期望行为:只渲染有界预览,HTML < 50k 且带「查看全文」。
run('a 24.6M character legacy message renders only a bounded preview with a view-all action', () => {
  const huge = '<tr><td>row</td></tr>\n'.repeat(Math.ceil(24_597_780 / 22)).slice(0, 24_597_780);
  const html = render({ content: huge });
  assert.ok(html.length < 50_000, `html length ${html.length}`);
  assert.match(html, /查看全文/);
  assert.match(html, /data-user-message-view-full="true"/);
});

// 触发场景:结构化消息的 text 部件超长(本版之前的大段粘贴存成一个 text 部件)。
// 期望行为:同样只渲染预览并提供「查看全文」。
run('an oversized composer text part is previewed too', () => {
  const html = render({ composerContent: { version: 1, parts: [{ type: 'text', text: 'q'.repeat(200_000) }] } });
  assert.ok(html.length < 50_000);
  assert.match(html, /查看全文/);
});

// 触发场景:19,999 字符的普通消息(预览上限以内)。
// 期望行为:完整渲染,没有「查看全文」。
run('a 19,999 character message renders in full', () => {
  const text = 'w'.repeat(19_999);
  const html = render({ content: text });
  assert.ok(html.includes(text));
  assert.doesNotMatch(html, /查看全文/);
});

// ---- PastedTextDialog(真实组件,极简 hook 驱动) ----

function compileDialog(contextValue) {
  const hooks = hookHarness();
  const scope = vm.runInNewContext(`${dialogCode}; ({ PastedTextDialog, readPastedTextSource });`, {
    React,
    useTranslation() {}, useId: () => 'dialog-title', useRef: () => ({ current: null }),
    useState: hooks.useState, useMemo: (factory) => factory(),
    useEffect: (effect) => { effect(); },
    useContext: (context) => (context === LoaderContext ? contextValue : undefined),
    AttachmentTextLoaderContext: LoaderContext,
    requestAnimationFrame: () => 0, cancelAnimationFrame() {},
    Modal: ({ children }) => React.createElement('section', {}, children),
    VsIcon: () => null, toast() {}, copyTextToClipboard: async () => {},
    formatNumber: (value) => String(value), ...pastedText,
  });
  return { ...scope, hooks };
}

// 触发场景:对话框的来源是 {url}(文件块),调用方没有显式传 loader。
// 期望行为:用 AttachmentTextLoaderContext 的 loader 读取该 URL。
run('PastedTextDialog reads a url source through the context loader', () => {
  const calls = [];
  const loader = (url) => { calls.push(url); return new Promise(() => {}); };
  const { PastedTextDialog } = compileDialog(loader);
  PastedTextDialog({ title: 't', source: { url: '/api/sessions/s1/attachments/f1/blob' }, readOnly: true });
  assert.deepEqual(calls, ['/api/sessions/s1/attachments/f1/blob']);
});

// 触发场景:可编辑的内联块(未超过 50 万字符)。
// 期望行为:textarea 不受控(没有 value 属性)、有「保存」默认按钮;超过 50 万字符时降级只读,
// 提示用剪贴板替换、wrap=off、没有「保存」。
run('PastedTextDialog is editable up to 500k chars and degrades to read-only beyond', () => {
  const { PastedTextDialog } = compileDialog(contextLoader);
  const small = renderToStaticMarkup(PastedTextDialog({ title: 't', source: { text: 'hello\nworld' }, onSave() {} }));
  assert.match(small, /data-pasted-text-dialog="edit"/);
  assert.match(small, /<textarea(?![^>]*\svalue=)[^>]*>/);
  assert.match(small, /data-ace-dialog-primary="true"[^>]*>保存</);
  assert.match(small, /2 行 · 11 字符/);
  const { PastedTextDialog: LongDialog } = compileDialog(contextLoader);
  const long = renderToStaticMarkup(LongDialog({
    title: 't', source: { text: 'x'.repeat(pastedText.PASTED_TEXT_EDIT_MAX_CHARS + 1) }, onSave() {}, onReplace() {},
  }));
  assert.match(long, /data-pasted-text-dialog="view"/);
  assert.match(long, /wrap="off"/);
  assert.match(long, /内容超过 50 万字符/);
  assert.match(long, /用剪贴板内容替换/);
  assert.doesNotMatch(long, />保存</);
});
