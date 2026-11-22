import assert from 'node:assert/strict';
import {
  annotationElement,
  annotationState,
  createAnnotationOutbox,
} from './annotationOutbox.js';
import {
  composeAnnotationMessage,
  groupRecordsByPage,
  prepareAnnotationSubmission,
  annotationSendOutcome,
  findMessageByClientId,
} from './annotationSubmission.js';
import { parsePageAnnotationMessage } from './pageAnnotationMessage.js';

const record = (id, number, extra = {}) => ({
  id, number, status: 'pending', valid: true, document_id: 'p1:document:1',
  description: `第 ${number} 条`, targets: [{ id: 'target-1', tag: 'DIV' }],
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

// 组装 → 解析往返：正文与批注载荷能对上，编号进得了卡片。
const grouped = groupRecordsByPage(outbox.getSnapshot().records);
assert.equal(grouped.length, 1);
const message = composeAnnotationMessage('帮我把这两处改一下', [{
  url: 'https://b.example/', title: '结算页',
  annotations: [{ number: 1, element: 'div', description: '间距太小' }],
}]);
assert.match(message, /^帮我把这两处改一下\n\nPage annotations\n\{/);
const parsed = parsePageAnnotationMessage(message);
assert.equal(parsed.text, '帮我把这两处改一下');
assert.equal(parsed.pages[0].title, '结算页');
assert.deepEqual(parsed.pages[0].comments[0],
  { number: 1, element: 'div', description: '间距太小', pageTitle: '结算页', state: '' });

// prepareAnnotationSubmission：上传每页整屏图 → 正文追加载荷 → settle 删掉已发记录。
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
const sent = parsePageAnnotationMessage(prepared.text);
assert.deepEqual(sent.pages.map((page) => page.shot),
  ['page-annotations-c0ffee-1.png', 'page-annotations-c0ffee-2.png']);
assert.equal(sent.pages.length, 2, 'orphan 记录的页面也算一页');

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

console.log('[pass] annotation outbox aggregates per session and assembles one message');
