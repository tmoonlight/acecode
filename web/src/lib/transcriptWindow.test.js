import assert from 'node:assert/strict';
import {
  createTranscriptState,
  loadTranscriptHistory,
  reduceTranscriptEvent,
} from './sessionTranscript.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';
import {
  INITIAL_TAIL_ITEMS,
  REVEAL_CHUNK_ITEMS,
  initialWindowAnchorKey,
  reconcileTranscriptWindowAnchorKey,
  revealEarlierAnchorKey,
  transcriptWindowItemKey,
  windowTranscriptItems,
} from './transcriptWindow.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 造 n 条投影行:每 turnEvery 条一条 user 行,其余 assistant/tool 交替。
function makeItems(n, { turnEvery = 6, persisted = false, idOffset = 0 } = {}) {
  return Array.from({ length: n }, (_, i) => {
    const id = idOffset + i + 1;
    const messageId = persisted ? `persisted-${i}` : '';
    if (i % turnEvery === 0) {
      return { kind: 'msg', id, messageId, role: 'user', content: `u${i}` };
    }
    if (i % 2 === 0) return { kind: 'tool', id, name: 'bash' };
    return { kind: 'msg', id, messageId, role: 'assistant', content: `a${i}` };
  });
}

function indexForAnchor(items, anchorKey) {
  return items.findIndex((item) => transcriptWindowItemKey(item) === anchorKey);
}

function makePersistedMessages(turnCount) {
  const messages = [];
  for (let i = 0; i < turnCount; i += 1) {
    messages.push({
      id: `user-${i}`,
      role: 'user',
      content: `user message ${i}`,
      ts: i * 2 + 1,
    });
    messages.push({
      id: `assistant-${i}`,
      role: 'assistant',
      content: `assistant message ${i}`,
      ts: i * 2 + 2,
    });
  }
  return messages;
}

run('稳定 messageId 与临时 item id 使用不同命名空间', () => {
  const persistedKey = transcriptWindowItemKey({ id: 7, messageId: '42' });
  const temporaryKey = transcriptWindowItemKey({ id: 42, messageId: '' });
  assert.equal(persistedKey, 'message:42');
  assert.equal(temporaryKey, 'item:42');
  assert.notEqual(persistedKey, temporaryKey);
  assert.equal(transcriptWindowItemKey({}), null);
});

run('协调保留稳定 key,临时 key 失效时同步重选有界窗口', () => {
  const persisted = makeItems(300, { persisted: true });
  const persistedKey = initialWindowAnchorKey(persisted);
  const rebuiltPersisted = makeItems(300, { persisted: true, idOffset: 1000 });
  assert.equal(
    reconcileTranscriptWindowAnchorKey(rebuiltPersisted, persistedKey),
    persistedKey,
  );

  const temporary = makeItems(300);
  const temporaryKey = initialWindowAnchorKey(temporary);
  const rebuiltTemporary = makeItems(300, { idOffset: 1000 });
  const repairedKey = reconcileTranscriptWindowAnchorKey(rebuiltTemporary, temporaryKey);
  assert.notEqual(repairedKey, temporaryKey);
  assert.equal(repairedKey, initialWindowAnchorKey(rebuiltTemporary));
  assert.ok(windowTranscriptItems(rebuiltTemporary, repairedKey).hiddenCount > 0);
});

run('协调区分空 transcript、短会话与显式全量视图', () => {
  assert.equal(reconcileTranscriptWindowAnchorKey([], 'message:old'), undefined);
  assert.equal(reconcileTranscriptWindowAnchorKey([], null), undefined);
  assert.equal(reconcileTranscriptWindowAnchorKey(makeItems(3), undefined), null);

  const large = makeItems(300, { persisted: true });
  assert.equal(reconcileTranscriptWindowAnchorKey(large, null), null);
  assert.equal(windowTranscriptItems(large, null).visible.length, large.length);
});

run('条目不超过初始窗口时不启用窗口', () => {
  assert.equal(initialWindowAnchorKey(makeItems(INITIAL_TAIL_ITEMS)), null);
  assert.equal(initialWindowAnchorKey([]), null);
  assert.equal(initialWindowAnchorKey(makeItems(3), 120), null);
});

run('初始锚点选在窗口边界前最近的 user 行(回合边界)', () => {
  const items = makeItems(400, { turnEvery: 6 });
  const anchorKey = initialWindowAnchorKey(items, 120);
  const idx = indexForAnchor(items, anchorKey);
  assert.ok(idx > 0 && idx <= 400 - 120, `anchor idx=${idx} 应在边界或更早`);
  assert.equal(items[idx].role, 'user');
  // 边界之后第一条 user 行必须更晚 —— 证明选的是"最近"的一条
  const boundary = 400 - 120;
  const nearest = items.slice(0, boundary + 1).map((it, i) => ({ it, i }))
    .filter(({ it }) => it.kind === 'msg' && it.role === 'user').pop();
  assert.equal(idx, nearest.i);
});

run('无 user 行时退回边界行自身', () => {
  const items = Array.from({ length: 200 }, (_, i) => (
    { kind: 'msg', id: i + 1, role: 'assistant', content: `a${i}` }
  ));
  const anchorKey = initialWindowAnchorKey(items, 120);
  assert.equal(anchorKey, transcriptWindowItemKey(items[200 - 120]));
});

run('windowTranscriptItems 按锚切窗,锚丢失时 fail-open 全量', () => {
  const items = makeItems(300);
  const anchorKey = initialWindowAnchorKey(items, 120);
  const { visible, hiddenCount } = windowTranscriptItems(items, anchorKey);
  assert.equal(visible.length + hiddenCount, items.length);
  assert.ok(hiddenCount > 0);
  assert.equal(transcriptWindowItemKey(visible[0]), anchorKey);

  // 锚点为空 / 找不到 / 指向首行 → 全量
  assert.equal(windowTranscriptItems(items, null).hiddenCount, 0);
  assert.equal(windowTranscriptItems(items, 'no-such-id').hiddenCount, 0);
  assert.equal(windowTranscriptItems(items, transcriptWindowItemKey(items[0])).hiddenCount, 0);
});

run('流式追加不动锚点:窗口在尾部自然增长', () => {
  const items = makeItems(300);
  const anchorKey = initialWindowAnchorKey(items, 120);
  const before = windowTranscriptItems(items, anchorKey);
  const appended = items.concat([{ kind: 'msg', id: 9999, role: 'assistant', content: 'new' }]);
  const after = windowTranscriptItems(appended, anchorKey);
  assert.equal(after.hiddenCount, before.hiddenCount);
  assert.equal(after.visible.length, before.visible.length + 1);
});

run('revealEarlierAnchorKey 按块向前并对齐 user 行,到头返回 null', () => {
  const items = makeItems(1000, { turnEvery: 6 });
  const first = initialWindowAnchorKey(items, 120);
  const next = revealEarlierAnchorKey(items, first, 240);
  assert.ok(next !== null);
  const firstIdx = indexForAnchor(items, first);
  const nextIdx = indexForAnchor(items, next);
  assert.ok(nextIdx < firstIdx && firstIdx - nextIdx >= 240);
  assert.equal(items[nextIdx].role, 'user');

  // 反复揭示最终到头(null = 全量),且不会死循环
  let anchor = first;
  for (let i = 0; i < 50 && anchor !== null; i += 1) {
    anchor = revealEarlierAnchorKey(items, anchor, 240);
  }
  assert.equal(anchor, null);

  // 剩余不足一个 chunk → 直接全量
  const shortItems = makeItems(300);
  const shortAnchor = initialWindowAnchorKey(shortItems, 120);
  assert.equal(revealEarlierAnchorKey(shortItems, shortAnchor, REVEAL_CHUNK_ITEMS), null);
});

run('transcript_replace 重建临时 id 后保留稳定有界窗口', () => {
  const messages = makePersistedMessages(140);
  const loaded = loadTranscriptHistory(createTranscriptState(), { messages });
  const beforeItems = projectCollapsedTranscriptItems(loaded.state.items);
  const anchorKey = initialWindowAnchorKey(beforeItems);
  const beforeWindow = windowTranscriptItems(beforeItems, anchorKey);
  const beforeAnchor = beforeItems[indexForAnchor(beforeItems, anchorKey)];

  assert.ok(beforeItems.length > INITIAL_TAIL_ITEMS);
  assert.match(anchorKey, /^message:/);
  assert.ok(beforeWindow.hiddenCount > 0);
  assert.ok(beforeWindow.visible.length <= INITIAL_TAIL_ITEMS + 1);

  const replaced = reduceTranscriptEvent(loaded.state, {
    type: 'transcript_replace',
    seq: 1,
    payload: { messages },
  }).state;
  const afterItems = projectCollapsedTranscriptItems(replaced.items);
  const resolvedKey = reconcileTranscriptWindowAnchorKey(afterItems, anchorKey);
  const afterWindow = windowTranscriptItems(afterItems, resolvedKey);
  const afterAnchor = afterItems[indexForAnchor(afterItems, resolvedKey)];

  assert.notEqual(afterAnchor.id, beforeAnchor.id, 'reducer 应已重建临时 item id');
  assert.equal(afterAnchor.messageId, beforeAnchor.messageId);
  assert.equal(resolvedKey, anchorKey);
  assert.ok(afterWindow.hiddenCount > 0);
  assert.equal(afterWindow.hiddenCount + afterWindow.visible.length, afterItems.length);
  assert.ok(afterWindow.visible.length <= INITIAL_TAIL_ITEMS + 1);
  assert.equal(transcriptWindowItemKey(afterWindow.visible[0]), anchorKey);

  // 用户显式展开全量后,同一替换投影不得重新收回窗口。
  const explicitFullKey = reconcileTranscriptWindowAnchorKey(afterItems, null);
  assert.equal(explicitFullKey, null);
  assert.equal(windowTranscriptItems(afterItems, explicitFullKey).hiddenCount, 0);
});

// 架构守护:ChatView 的 transcript 行渲染必须走窗口化列表,不能退回全量 map。
const chatViewSource = (await import('node:fs')).readFileSync(
  new URL('../components/ChatView.jsx', import.meta.url),
  'utf8',
).replace(/\r\n?/g, '\n');
run('架构: ChatView 行渲染使用 windowedItems,保留揭示入口与滚动补偿', () => {
  assert.match(chatViewSource, /windowedItems\.map\(\(it\)/);
  assert.doesNotMatch(chatViewSource, /renderedItems\.map\(\(it\)/);
  assert.match(chatViewSource, /revealEarlierTranscript/);
  assert.match(chatViewSource, /windowRevealScrollRef/);
  assert.match(chatViewSource, /reconcileTranscriptWindowAnchorKey/);
  assert.ok(
    chatViewSource.indexOf('reconcileTranscriptWindowAnchorKey(')
      < chatViewSource.indexOf('windowTranscriptItems(renderedItems, windowAnchorKey)'),
    'ChatView 必须先协调 key,再切片当前 render',
  );
});
