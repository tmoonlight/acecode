import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  annotationElement,
  annotationState,
  createAnnotationOutbox,
} from './annotationOutbox.js';
import {
  annotationContext,
  groupRecordsByPage,
  prepareAnnotationSubmission,
  annotationSendOutcome,
  findMessageByClientId,
  registerAnnotationSubmitter,
  requestAnnotationSubmit,
} from './annotationSubmission.js';
import { composerContentFromText, composerContentSubmissionText } from './composerContent.js';
import { normalizeComposerPayload } from './composerSubmission.js';
import {
  pageAnnotationCardFromContexts, pageAnnotationMessageCopyText,
} from './pageAnnotationMessage.js';

const record = (id, number, extra = {}) => ({
  id, number, status: 'pending', valid: true, document_id: 'p1:document:1',
  description: `第 ${number} 条`,
  targets: [{ id: 'target-1', tag: 'DIV', locator: 'div#card-1', text: '卡片标题' }],
  differences: [], region: null, ...extra,
});

const outbox = createAnnotationOutbox();
outbox.observe({
  pageId: 'p1', url: 'https://a.example/', title: '商品页',
  snapshot: { document_id: 'p1:document:1', records: [record('p1:annotation:1', 1)] },
});
outbox.observe({
  pageId: 'p2', url: 'https://b.example/', title: '结算页',
  snapshot: { document_id: 'p2:document:1', records: [record('p2:annotation:1', 1)] },
});
assert.equal(outbox.pendingCount(), 2);
assert.deepEqual(outbox.getSnapshot().records.map((item) => item.id),
  ['p1:annotation:1', 'p2:annotation:1'], '按页签顺序聚合，跨页签可见');
assert.deepEqual(outbox.getSnapshot().pages.map((page) => page.count), [1, 1]);
assert.equal(annotationElement({ targets: [{ tag: 'INPUT' }] }), 'input');

// 关页签：记录留下，状态降级为 orphan，图还在。
outbox.observe({ pageId: 'p2', closed: true, snapshot: null });
assert.equal(outbox.pendingCount(), 2, '关页签不丢记录');
assert.equal(outbox.getSnapshot().records.find((item) => item.pageId === 'p2').state, 'orphan');
assert.equal(outbox.getSnapshot().pages.find((page) => page.id === 'p2').closed, true);

// 文档换了但页签活着 → replayable；目标失效 → stale。
assert.equal(annotationState({ valid: true, document_id: 'p1:document:1' },
  { closed: false, documentId: 'p1:document:2' }), 'replayable');
assert.equal(annotationState({ valid: false, document_id: 'p1:document:1' },
  { closed: false, documentId: 'p1:document:1' }), 'stale');

outbox.markSent(['p1:annotation:1']);
assert.equal(outbox.pendingCount(), 1);
assert.equal(outbox.getSnapshot().records[0].id, 'p2:annotation:1');

// 卡片数据独立于正文：contexts 携带结构化批注，正文不再拼 JSON。
const grouped = groupRecordsByPage(outbox.getSnapshot().records);
assert.equal(grouped.length, 1);
const context = annotationContext([{
  url: 'https://b.example/', title: '结算页',
  annotations: [{ number: 1, element: 'div', description: '间距太小' }],
}]);
assert.equal(context.type, 'page_annotations');
assert.equal(context.version, 2);
const contextCard = pageAnnotationCardFromContexts([{ type: 'browser_context', context }]);
assert.equal(contextCard.text, '');
assert.equal(contextCard.pages[0].title, '结算页');
assert.deepEqual(contextCard.pages[0].comments[0],
  { number: 1, element: 'div', description: '间距太小', pageTitle: '结算页', state: '' });
assert.equal(pageAnnotationMessageCopyText(contextCard, '帮我把这两处改一下'),
  '帮我把这两处改一下\n\ndiv 间距太小');

// prepareAnnotationSubmission：上传每页整屏图 → 批注进 contexts → settle 删掉已发记录。
const calls = [];
const uploaded = [];
const api = {
  uploadSessionAttachment: async (sessionId, body) => {
    uploaded.push({ sessionId, body });
    return { attachment: { id: `att-${uploaded.length}` } };
  },
};
const box = createAnnotationOutbox();
box.observe({
  pageId: 'p1', url: 'https://a.example/', title: '商品页',
  snapshot: { document_id: 'p1:document:1', page_shot: 'data:image/png;base64,AAA', records: [record('p1:annotation:1', 1)] },
});
box.observe({
  pageId: 'p2', url: 'https://b.example/', title: '结算页',
  snapshot: { document_id: 'p2:document:1', page_shot: 'data:image/png;base64,BBB', records: [record('p2:annotation:1', 1)] },
});
box.closePage('p2');
const prepared = await prepareAnnotationSubmission({
  sessionId: 's1', text: '改一下', records: box.pendingRecords(), outbox: box, api,
  makeId: () => 'c0ffee',
  invoke: async (pageId, command) => { calls.push({ pageId, command }); return { ok: true }; },
});
assert.equal(uploaded.length, 2, '每页上传一张整屏图');
assert.equal(uploaded[0].body.data_base64, 'AAA', '去掉 data URL 前缀再上传');
assert.equal(uploaded[0].body.name, 'page-annotations-c0ffee-1.png');
assert.equal(prepared.shotAttachments.length, 2);
assert.equal(prepared.attachments.length, 2);
assert.equal(prepared.text, '改一下', '正文保持用户输入，不拼载荷');
assert.doesNotMatch(prepared.text, /Page annotations|"pages"/,
  '聊天正文不能泄漏批注 JSON');
assert.equal(prepared.contexts.length, 1);
const sent = pageAnnotationCardFromContexts(
  prepared.contexts.map((item) => ({ type: 'browser_context', context: item })),
);
assert.deepEqual(sent.pages.map((page) => page.shot),
  ['page-annotations-c0ffee-1.png', 'page-annotations-c0ffee-2.png']);
assert.equal(sent.pages.length, 2, 'orphan 记录的页面也算一页');
const payload = prepared.contexts[0];
assert.deepEqual(payload.pages.map((page) => page.page_id), ['p1', 'p2'],
  'the model needs the live page id to keep using browser tools on the annotated page');
const structured = payload.pages[0].annotations[0];
assert.equal(structured.description, '第 1 条');
assert.equal(structured.element, 'div');
assert.equal(structured.targets[0].tag, 'DIV');
assert.equal(structured.targets[0].locator, 'div#card-1');
assert.equal(structured.targets[0].text, '卡片标题');
const staleContent = normalizeComposerPayload(
  prepared.text, prepared.attachments, prepared.contexts, null, composerContentFromText(''),
);
assert.equal(composerContentSubmissionText(staleContent.composer_content), '');
assert.equal(staleContent.text, '改一下',
  'annotation contexts must leave the natural-language body intact');
assert.equal(staleContent.contexts[0].type, 'page_annotations');
assert.equal(staleContent.contexts[0].version, 2);
assert.deepEqual(staleContent.contexts[0].pages, prepared.contexts[0].pages,
  'composer normalization must preserve annotation pages for the card and model context');
assert.equal(pageAnnotationCardFromContexts(
  staleContent.contexts.map((item) => ({ type: 'browser_context', context: item })),
).pages.length, 2, 'the normalized payload still renders the sent annotation card');
const flattened = normalizeComposerPayload(prepared.text, prepared.attachments, prepared.contexts, null, null);
assert.equal(flattened.text, prepared.text);
assert.equal(flattened.contexts[0].type, 'page_annotations');
assert.equal(flattened.contexts[0].pages.length, 2);
assert.equal('composer_content' in flattened, false,
  'annotation messages must not send stale editor content');

prepared.settle(false);
assert.equal(box.pendingCount(), 2, '未确认的发送不清空待发批注');
prepared.settle(true);
await new Promise((resolve) => setTimeout(resolve, 0));
assert.equal(box.pendingCount(), 0);
assert.equal(calls.length, 1, '只为活着的页签发删除命令');
assert.equal(calls[0].pageId, 'p1');
assert.deepEqual(calls[0].command.record_ids, ['p1:annotation:1']);
assert.equal(calls[0].command.action, 'remove');

// 超时自查（#215）：40 秒没确认时按 client_message_id 反查，查到算成功。
const fast = await annotationSendOutcome({
  api: {}, sessionId: 's1', clientMessageId: 'c1', send: Promise.resolve({ queued: true }),
});
assert.equal(fast.status, 'accepted');
assert.deepEqual(fast.value, { queued: true });

const hanging = () => new Promise(() => {});
const apiWith = (messages) => ({ getMessages: async () => ({ messages }) });
assert.equal((await annotationSendOutcome({
  api: apiWith([{ metadata: { client_message_id: 'c1' } }]),
  sessionId: 's1', clientMessageId: 'c1', send: hanging(), timeoutMs: 10,
})).status, 'confirmed', '查到了就当成功，不重发');
assert.equal((await annotationSendOutcome({
  api: apiWith([{ metadata: { client_message_id: 'other' } }]),
  sessionId: 's1', clientMessageId: 'c1', send: hanging(), timeoutMs: 10,
})).status, 'lost', '查不到就退回待发');
assert.equal((await annotationSendOutcome({
  api: { getMessages: async () => { throw new Error('network'); } },
  sessionId: 's1', clientMessageId: 'c1', send: hanging(), timeoutMs: 10,
})).status, 'unknown', '查询本身失败时不回滚');
assert.equal(await findMessageByClientId(apiWith([{ metadata: { client_message_id: 'c9' } }]), 's1', 'c9'), true);
assert.equal(await findMessageByClientId(apiWith([]), 's1', 'c9'), false);

// 无页签弹窗保存后，卡片读取的待发箱必须立即带上新描述；页签稍后的旧快照
// 不能把它恢复成旧文字。
const closedBox = createAnnotationOutbox();
closedBox.observe({
  pageId: 'closed-page', url: 'https://closed.example/', title: '已关闭页', closed: true,
  snapshot: { records: [record('closed-page:annotation:1', 1, { description: '旧描述' })] },
});
assert.equal(closedBox.updateDescription('closed-page:annotation:1', '新描述'), true);
assert.equal(closedBox.getSnapshot().records[0].description, '新描述');
closedBox.observe({
  pageId: 'closed-page', url: 'https://closed.example/', title: '已关闭页', closed: true,
  snapshot: { records: [record('closed-page:annotation:1', 1, { description: '旧描述' })] },
});
assert.equal(closedBox.getSnapshot().records[0].description, '新描述',
  'local no-page edits survive a closed page snapshot');
closedBox.discardPage('closed-page');
closedBox.observe({
  pageId: 'closed-page', url: 'https://closed.example/', title: '已关闭页',
  snapshot: { records: [record('closed-page:annotation:1', 1, { description: '重新加载的描述' })] },
});
assert.equal(closedBox.getSnapshot().records[0].description, '重新加载的描述',
  'discarding a page drops local description overrides');

// 触发器只认结构化结果:抛错、返回 undefined、明确失败都不能当成已发送。
const stopFalse = registerAnnotationSubmitter('registry-false', async () => ({ ok: false, error: 'nope' }));
assert.deepEqual(await requestAnnotationSubmit('registry-false'), { ok: false, error: 'nope' });
stopFalse();
const stopThrow = registerAnnotationSubmitter('registry-throw', async () => { throw new Error('offline'); });
assert.deepEqual(await requestAnnotationSubmit('registry-throw'), { ok: false, error: 'offline' });
stopThrow();
const stopVoid = registerAnnotationSubmitter('registry-void', () => {});
assert.deepEqual(await requestAnnotationSubmit('registry-void'), { ok: false, error: '发送失败' });
stopVoid();
assert.deepEqual(await requestAnnotationSubmit('registry-missing'), {
  ok: false, error: '请先打开一个对话再发送批注',
});

const chatView = fs.readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
assert.match(chatView, /result = await submit\(prepared\.text/,
  'annotation sends must await the real composer submission');
assert.match(chatView, /extraContexts: prepared\.contexts/,
  'annotation data must travel through the browser-context card channel');
assert.match(chatView, /if \(result\?\.ok === false\) return result;/);
assert.match(chatView, /settleAnnotations\('unknown'\)/,
  'an unconfirmed send must keep records pending');
assert.match(chatView, /hasPendingAnnotations=\{annotationState\.pendingCount > 0\}/);
assert.match(chatView, /annotationSettle \? null : submittedContent/,
  'annotation sends must not let stale composer_content replace the submitted text');
assert.match(chatView, /composerContentSubmissionText\(composerContentRef\.current\) \|\| value/,
  'annotation sends must merge structured draft text before adding annotations');
assert.match(chatView, /requestAnnotationEdit\(record\.pageId, record, \{[\s\S]{0,400}pages: sessionBrowserPages,[\s\S]{0,400}tabs: previewTabs,[\s\S]{0,400}activate: \(pageId\) => showBrowserPage\(pageId\)/,
  'editing an unfocused tab must resolve and activate the live browser page');
assert.match(chatView, /result\.reason === 'missing_page'/,
  'only a genuinely missing page may open the no-page editor');

console.log('[pass] annotation outbox aggregates per session and assembles one message');
