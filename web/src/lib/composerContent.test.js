import assert from 'node:assert/strict';
import {
  PASTED_TEXT_SEPARATOR, appendComposerText,
  cloneComposerContent, composerAttachmentKey, composerContentAttachments,
  composerContentClipboardText, composerContentFromMessage, composerContentFromText,
  composerContentHasPastedText, composerContentLeadsWithPastedText,
  composerContentSignature, composerContentSubmissionText, composerContentText,
  isPasteBlockPart, normalizeComposerContent, reconcileComposerContentAttachments,
} from './composerContent.js';

function run(name, fn) { fn(); console.log(`[pass] ${name}`); }
const mixed = {
  version: 1,
  parts: [
    { type: 'text', text: 'Use ' },
    { type: 'skill', name: 'review', token: '$review', path: '/skills/review/SKILL.md' },
    { type: 'text', text: ' on ' },
    { type: 'path', path: 'src/a.cpp', token: '@src/a.cpp', directory: false },
    { type: 'text', text: ' with ' },
    { type: 'attachment', key: 'upload-1', id: 'a1', name: 'diagram.png', kind: 'image' },
    { type: 'text', text: '\nthen explain.' },
  ],
};

run('ordered content round trips without editor objects or aliasing', () => {
  const copy = cloneComposerContent(JSON.parse(JSON.stringify(mixed)));
  assert.deepEqual(copy, mixed);
  assert.equal(composerContentText(copy), 'Use $review on @src/a.cpp with \nthen explain.');
  copy.parts[0].text = 'Changed';
  assert.equal(mixed.parts[0].text, 'Use ');
  assert.notEqual(composerContentSignature(copy), composerContentSignature(mixed));
});

run('canonical text coalescing preserves line breaks and empty documents', () => {
  assert.deepEqual(normalizeComposerContent({ version: 1, parts: [
    { type: 'text', text: 'a\r\n' }, { type: 'text', text: '' }, { type: 'text', text: 'b' },
  ] }), { version: 1, parts: [{ type: 'text', text: 'a\nb' }] });
  assert.deepEqual(composerContentFromText(''), { version: 1, parts: [] });
  assert.equal(composerContentSignature(null), '');
  assert.notEqual(composerContentSignature(composerContentFromText('')), '');
});

run('unsupported or malformed content is rejected without silently deleting references', () => {
  for (const value of [null, {}, { version: 2, parts: [] }, { version: 1, parts: [{}] },
    { version: 1, parts: [{ type: 'path', token: '@a' }] },
    { version: 1, parts: [{ type: 'skill', name: 'review' }] },
    { version: 1, parts: [{ type: 'attachment', name: 'lost.png' }] },
    { version: 1, parts: [{ type: 'text', text: 3 }] },
  ]) assert.equal(normalizeComposerContent(value), null);
});

run('legacy text and attachments get a deterministic fallback without parsing normal prose', () => {
  const fallback = composerContentFromText('Keep /unknown and $ordinary literal', [{ id: 'old', name: 'old.pdf' }]);
  assert.equal(fallback.parts[0].type, 'attachment');
  assert.equal(fallback.parts[0].key, 'old');
  assert.equal(fallback.parts[1].text, 'Keep /unknown and $ordinary literal');
  assert.equal(composerAttachmentKey({ local_id: 'local', id: 'uploaded' }), 'local');
});

run('upload hydration changes metadata in place and cannot resurrect deleted references', () => {
  const pending = composerContentFromText('tail', [{ local_id: 'upload-1', name: 'diagram.png', uploading: true }]);
  pending.parts.unshift({ type: 'text', text: 'head ' });
  const records = [{ local_id: 'upload-1', id: 'a1', name: 'diagram.png', kind: 'image', mime_type: 'image/png' }];
  const hydrated = reconcileComposerContentAttachments(pending, records);
  assert.deepEqual(hydrated.parts.map((part) => part.type), ['text', 'attachment', 'text']);
  assert.equal(hydrated.parts[1].key, 'upload-1');
  assert.equal(hydrated.parts[1].id, 'a1');
  assert.deepEqual(reconcileComposerContentAttachments(composerContentFromText('deleted'), records), composerContentFromText('deleted'));
});

run('draft attachment recovery and duplicate references retain resource identity', () => {
  const content = cloneComposerContent(mixed);
  content.parts.push({ ...mixed.parts[5], key: 'copied-reference' });
  const attachments = composerContentAttachments(content, [], { sessionId: 'session-1' });
  assert.equal(attachments.length, 1);
  assert.equal(attachments[0].local_id, 'upload-1');
  assert.equal(attachments[0].blob_url, '/api/sessions/session-1/attachments/a1/blob');
  const withRecord = composerContentAttachments(content, [{ id: 'a1', blob_url: '/actual/blob', metadata: { source_path: '/original.png' } }]);
  assert.equal(withRecord[0].blob_url, '/actual/blob');
  assert.equal(withRecord[0].metadata.source_path, '/original.png');
});

run('unfinished live uploads remain usable but restored missing resources cannot be sent silently', () => {
  const content = composerContentFromText('', [{ local_id: 'pending', name: 'a.png' }]);
  assert.ok(composerContentAttachments(content)[0].error);
  const pending = composerContentAttachments(content, [{ local_id: 'pending', uploading: true, pending_upload: true }])[0];
  assert.equal(pending.error, undefined);
  assert.equal(pending.uploading, true);
});

run('clipboard and transcript helpers preserve inline attachment labels and wire/frontend formats', () => {
  assert.equal(composerContentClipboardText(mixed), 'Use $review on @src/a.cpp with [diagram.png]\nthen explain.');
  for (const message of [{ composerContent: mixed }, { composer_content: mixed }, { metadata: { composer_content: mixed } }]) {
    assert.deepEqual(composerContentFromMessage(message), mixed);
  }
  assert.equal(composerContentFromMessage({ content: 'legacy' }), null);
});

// ---- 粘贴块(第 2 条反馈 f300) ----
const pasteFile = (extra = {}) => ({
  type: 'attachment', key: 'paste-file-1', id: 'att-1', name: 'pasted-text-20260924-101500.txt',
  kind: 'file', mime_type: 'text/plain', paste: { title: '日志开头', chars: 10, lines: 2 }, ...extra,
});

// 触发场景:normalize 遇到内联粘贴块。
// 期望:key 与正文原样保留(不做 CRLF 改写、不与相邻 text 合并),非字符串正文 / 非字符串 key
// 拒绝整份文档,空块丢弃。
run('pasted_text parts keep key and body verbatim and never merge with text', () => {
  const content = normalizeComposerContent({ version: 1, parts: [
    { type: 'text', text: 'a' },
    { type: 'pasted_text', key: 'k1', text: 'L1\r\nL2' },
    { type: 'text', text: 'b' },
    { type: 'pasted_text', key: 'empty', text: '' },
  ] });
  assert.deepEqual(content.parts, [
    { type: 'text', text: 'a' },
    { type: 'pasted_text', key: 'k1', text: 'L1\r\nL2' },
    { type: 'text', text: 'b' },
  ]);
  assert.equal(normalizeComposerContent({ version: 1, parts: [{ type: 'pasted_text', text: 3 }] }), null);
  assert.equal(normalizeComposerContent({ version: 1, parts: [{ type: 'pasted_text', key: 7, text: 'x' }] }), null);
  assert.deepEqual(normalizeComposerContent({ version: 1, parts: [{ type: 'pasted_text', text: 'x' }] }).parts,
    [{ type: 'pasted_text', text: 'x' }]);
  assert.equal(isPasteBlockPart(content.parts[1]), true);
  assert.equal(isPasteBlockPart(content.parts[0]), false);
  assert.equal(isPasteBlockPart(pasteFile()), true);
});

// 触发场景:编辑器文本与提交正文。
// 期望:编辑器文本不含任何粘贴块(保住 value === composerContentText(content));正文用 C1 的三组
// 样例,与 tests/session/composer_content_test.cpp 的 PastedText* 用例逐字节对齐。
run('editor text excludes blocks and submission text matches the C++ separator samples', () => {
  const sample1 = { version: 1, parts: [{ type: 'text', text: '分析下面日志' }, { type: 'pasted_text', key: 'p', text: 'L1\nL2' }] };
  assert.equal(composerContentText(sample1), '分析下面日志');
  assert.equal(composerContentSubmissionText(sample1), '分析下面日志\n\nL1\nL2');
  const sample2 = { version: 1, parts: [
    { type: 'pasted_text', key: 'a', text: 'A' }, { type: 'text', text: 'B' }, { type: 'pasted_text', key: 'c', text: 'C' },
  ] };
  assert.equal(composerContentText(sample2), 'B');
  assert.equal(composerContentSubmissionText(sample2), 'A\n\nB\n\nC');
  const sample3 = { version: 1, parts: [{ type: 'pasted_text', key: 'a', text: 'A' }] };
  assert.equal(composerContentSubmissionText(sample3), 'A');
  assert.equal(PASTED_TEXT_SEPARATOR, '\n\n');
  // 文件块不进正文;附件不打断「上一片段是块」的状态(与 C++ append_submission 相同)。
  const withFile = { version: 1, parts: [
    { type: 'pasted_text', key: 'a', text: 'A' }, pasteFile(), { type: 'text', text: 'B' },
  ] };
  assert.equal(composerContentSubmissionText(withFile), 'A\n\nB');
  assert.equal(composerContentText(withFile), 'B');
});

// 触发场景:签名比较(每次按键都跑)。
// 期望:带 key 的内联块只看 key + 长度;同 key 同长度视为未变,换 key(编辑必换 key)即不同;
// 签名里不嵌入 30 万字符的正文。
run('signature of keyed inline blocks uses key and length only', () => {
  const a = { version: 1, parts: [{ type: 'pasted_text', key: 'k', text: 'x'.repeat(300000) }] };
  const b = { version: 1, parts: [{ type: 'pasted_text', key: 'k', text: 'y'.repeat(300000) }] };
  const c = { version: 1, parts: [{ type: 'pasted_text', key: 'k2', text: 'x'.repeat(300000) }] };
  assert.equal(composerContentSignature(a), composerContentSignature(b));
  assert.notEqual(composerContentSignature(a), composerContentSignature(c));
  assert.ok(composerContentSignature(a).length < 200, 'signature must not embed the block body');
});

// 触发场景:判断「编辑器为空、只有粘贴块」(D9 ①:此时一律普通消息)。
// 期望:编辑器只有空白 + 任一块(内联或文件)为 true;编辑器有非空文本或没有块为 false。
// HasPastedText 只认内联块(文件块是附件,本来就算 extras)。
run('leading paste detection and inline detection', () => {
  const inlineOnly = { version: 1, parts: [{ type: 'text', text: '  \n' }, { type: 'pasted_text', key: 'p', text: '/compact now' }] };
  assert.equal(composerContentLeadsWithPastedText(inlineOnly), true);
  assert.equal(composerContentLeadsWithPastedText({ version: 1, parts: [pasteFile()] }), true);
  assert.equal(composerContentLeadsWithPastedText({ version: 1, parts: [{ type: 'text', text: '/goal x' }, pasteFile()] }), false);
  assert.equal(composerContentLeadsWithPastedText(composerContentFromText('')), false);
  assert.equal(composerContentHasPastedText(inlineOnly), true);
  assert.equal(composerContentHasPastedText({ version: 1, parts: [pasteFile()] }), false);
});

// 触发场景:attachment 部件带 paste / store 描述。
// 期望:paste 清洗(未知键丢弃、非法计数丢弃、part > parts 时丢掉分段信息);store 必须是
// workspace_draft 且带合法 scope([A-Za-z0-9_]{1,128},同 C1),否则 store / store_scope 都丢。
run('paste descriptor and workspace draft store are cleaned', () => {
  const [part] = normalizeComposerContent({ version: 1, parts: [pasteFile({
    paste: { title: 'T', chars: 5, lines: -1, part: 1, parts: 2, extra: true },
    store: 'workspace_draft', store_scope: '__no_workspace__',
  })] }).parts;
  assert.deepEqual(part.paste, { title: 'T', chars: 5, part: 1, parts: 2 });
  assert.equal(part.store, 'workspace_draft');
  assert.equal(part.store_scope, '__no_workspace__');
  const [bad] = normalizeComposerContent({ version: 1, parts: [pasteFile({
    paste: { title: 'T', part: 3, parts: 2 }, store: 'workspace_draft',
  })] }).parts;
  assert.deepEqual(bad.paste, { title: 'T' });
  assert.equal('store' in bad, false, 'store without scope is dropped');
  for (const extra of [{ store: 'other', store_scope: 'abc' }, { store: 'workspace_draft', store_scope: '../x' }, { store_scope: 'abc' }]) {
    const [cleaned] = normalizeComposerContent({ version: 1, parts: [pasteFile(extra)] }).parts;
    assert.equal('store' in cleaned, false);
    assert.equal('store_scope' in cleaned, false);
  }
});

// 触发场景:上传回填 / 导入 / 草稿恢复时按附件记录 reconcile。
// 期望:paste 以部件为准保留(记录上没有也不丢);store 由记录的 session_id 推导 —— 记录属于真实会话
// 时清掉 store,属于 .workspace-draft 时补上 store(scope 取部件),记录没有 session_id(从草稿部件
// 重建的资源)时保留部件上的值。回归意义:部件上的 store 只是镜像,不能以客户端标记为准。
run('reconcile keeps paste from the part and derives store from the record owner', () => {
  const draftPart = pasteFile({ id: '', store: 'workspace_draft', store_scope: 'abc' });
  const content = { version: 1, parts: [{ type: 'text', text: 'x' }, draftPart] };
  const imported = reconcileComposerContentAttachments(content, [{ local_id: 'paste-file-1', id: 'new', session_id: 'sess-1', name: 'n.txt', mime_type: 'text/plain' }]);
  assert.equal(imported.parts[1].id, 'new');
  assert.deepEqual(imported.parts[1].paste, draftPart.paste);
  assert.equal('store' in imported.parts[1], false);
  assert.equal('store_scope' in imported.parts[1], false);

  const uploaded = reconcileComposerContentAttachments({ version: 1, parts: [pasteFile({ id: '', store: 'workspace_draft', store_scope: 'abc' })] },
    [{ local_id: 'paste-file-1', id: 'd1', session_id: '.workspace-draft', name: 'n.txt' }]);
  assert.equal(uploaded.parts[0].id, 'd1');
  assert.equal(uploaded.parts[0].store, 'workspace_draft');
  assert.equal(uploaded.parts[0].store_scope, 'abc');
  const unscoped = reconcileComposerContentAttachments({ version: 1, parts: [pasteFile({ id: '' })] },
    [{ local_id: 'paste-file-1', id: 'd1', session_id: '.workspace-draft', name: 'n.txt', store_scope: 'xyz' }]);
  assert.equal(unscoped.parts[0].store, 'workspace_draft', 'draft owner adds store, scope falls back to the record');
  assert.equal(unscoped.parts[0].store_scope, 'xyz');

  const rebuilt = reconcileComposerContentAttachments({ version: 1, parts: [draftPart] }, [{ local_id: 'paste-file-1', name: 'n.txt' }]);
  assert.equal(rebuilt.parts[0].store, 'workspace_draft');
  assert.equal(rebuilt.parts[0].store_scope, 'abc');
});

// 触发场景:由草稿部件 / 草稿附件记录合成附件资源(草稿恢复、payload 组装)。
// 期望:store 项带 paste / store / store_scope,且不输出 blob_url —— 持久化的
// /api/sessions/.workspace-draft/… 不是可用路由,即使传了 sessionId 也不拼会话 URL。
run('workspace draft attachments carry store fields and never a blob_url', () => {
  const content = { version: 1, parts: [pasteFile({ id: 'd1', store: 'workspace_draft', store_scope: 'abc' })] };
  const [fromPart] = composerContentAttachments(content, [], { sessionId: 'sess-1' });
  assert.equal(fromPart.store, 'workspace_draft');
  assert.equal(fromPart.store_scope, 'abc');
  assert.deepEqual(fromPart.paste, content.parts[0].paste);
  assert.equal(fromPart.blob_url, undefined);
  const [fromRecord] = composerContentAttachments(content, [{ id: 'd1', session_id: '.workspace-draft', blob_url: '/api/sessions/.workspace-draft/attachments/d1/blob' }]);
  assert.equal(fromRecord.blob_url, undefined);
  assert.equal(fromRecord.store, 'workspace_draft');
  const [session] = composerContentAttachments({ version: 1, parts: [pasteFile()] }, [], { sessionId: 'sess-1' });
  assert.equal(session.blob_url, '/api/sessions/sess-1/attachments/att-1/blob');
  assert.equal(session.store, undefined);
  assert.deepEqual(session.paste, pasteFile().paste);
});

// 触发场景:复制含粘贴块的消息 / 输入。
// 期望:内联块取全文(按正文分隔规则),文件块输出 [粘贴的文本: 标题],普通附件仍是 [文件名]。
run('clipboard text includes inline bodies and labels file blocks', () => {
  const content = { version: 1, parts: [
    { type: 'text', text: '看下' }, { type: 'pasted_text', key: 'p', text: 'BODY' }, pasteFile(),
  ] };
  assert.equal(composerContentClipboardText(content), '看下\n\nBODY\n\n[粘贴的文本: 日志开头]');
});

// 触发场景:会话正忙,用户点了对话内可视化(```visual)里的追问按钮,文字只填进输入框。
// 期望:输入框为空时直接成为全部文字;已有文字时换行追加;附件、技能、路径等其它部分
// 原样保留且顺序不变(覆盖 content 会丢掉用户已经放进去的附件)。
run('append text keeps existing parts and separates it with a newline', () => {
  assert.equal(composerContentText(appendComposerText(null, '讲讲缓存层')), '讲讲缓存层');
  const appended = appendComposerText(mixed, '讲讲缓存层');
  assert.equal(composerContentText(appended), 'Use $review on @src/a.cpp with \nthen explain.\n讲讲缓存层');
  assert.deepEqual(appended.parts.slice(0, -1), mixed.parts.slice(0, -1));
  assert.equal(appended.parts.filter((part) => part.type === 'attachment').length, 1);
  assert.deepEqual(appendComposerText(mixed, ''), cloneComposerContent(mixed));
});
