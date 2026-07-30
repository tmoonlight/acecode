import assert from 'node:assert/strict';
import {
  INITIAL_TAIL_ITEMS,
  REVEAL_CHUNK_ITEMS,
  initialWindowAnchorId,
  revealEarlierAnchorId,
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
function makeItems(n, { turnEvery = 6 } = {}) {
  return Array.from({ length: n }, (_, i) => {
    if (i % turnEvery === 0) return { kind: 'msg', id: i + 1, role: 'user', content: `u${i}` };
    if (i % 2 === 0) return { kind: 'tool', id: i + 1, name: 'bash' };
    return { kind: 'msg', id: i + 1, role: 'assistant', content: `a${i}` };
  });
}

run('条目不超过初始窗口时不启用窗口', () => {
  assert.equal(initialWindowAnchorId(makeItems(INITIAL_TAIL_ITEMS)), null);
  assert.equal(initialWindowAnchorId([]), null);
  assert.equal(initialWindowAnchorId(makeItems(3), 120), null);
});

run('初始锚点选在窗口边界前最近的 user 行(回合边界)', () => {
  const items = makeItems(400, { turnEvery: 6 });
  const anchorId = initialWindowAnchorId(items, 120);
  const idx = items.findIndex((it) => String(it.id) === anchorId);
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
  const anchorId = initialWindowAnchorId(items, 120);
  assert.equal(anchorId, String(items[200 - 120].id));
});

run('windowTranscriptItems 按锚切窗,锚丢失时 fail-open 全量', () => {
  const items = makeItems(300);
  const anchorId = initialWindowAnchorId(items, 120);
  const { visible, hiddenCount } = windowTranscriptItems(items, anchorId);
  assert.equal(visible.length + hiddenCount, items.length);
  assert.ok(hiddenCount > 0);
  assert.equal(String(visible[0].id), anchorId);

  // 锚点为空 / 找不到 / 指向首行 → 全量
  assert.equal(windowTranscriptItems(items, null).hiddenCount, 0);
  assert.equal(windowTranscriptItems(items, 'no-such-id').hiddenCount, 0);
  assert.equal(windowTranscriptItems(items, String(items[0].id)).hiddenCount, 0);
});

run('流式追加不动锚点:窗口在尾部自然增长', () => {
  const items = makeItems(300);
  const anchorId = initialWindowAnchorId(items, 120);
  const before = windowTranscriptItems(items, anchorId);
  const appended = items.concat([{ kind: 'msg', id: 9999, role: 'assistant', content: 'new' }]);
  const after = windowTranscriptItems(appended, anchorId);
  assert.equal(after.hiddenCount, before.hiddenCount);
  assert.equal(after.visible.length, before.visible.length + 1);
});

run('revealEarlierAnchorId 按块向前并对齐 user 行,到头返回 null', () => {
  const items = makeItems(1000, { turnEvery: 6 });
  const first = initialWindowAnchorId(items, 120);
  const next = revealEarlierAnchorId(items, first, 240);
  assert.ok(next !== null);
  const firstIdx = items.findIndex((it) => String(it.id) === first);
  const nextIdx = items.findIndex((it) => String(it.id) === next);
  assert.ok(nextIdx < firstIdx && firstIdx - nextIdx >= 240);
  assert.equal(items[nextIdx].role, 'user');

  // 反复揭示最终到头(null = 全量),且不会死循环
  let anchor = first;
  for (let i = 0; i < 50 && anchor !== null; i += 1) {
    anchor = revealEarlierAnchorId(items, anchor, 240);
  }
  assert.equal(anchor, null);

  // 剩余不足一个 chunk → 直接全量
  const shortItems = makeItems(300);
  const shortAnchor = initialWindowAnchorId(shortItems, 120);
  assert.equal(revealEarlierAnchorId(shortItems, shortAnchor, REVEAL_CHUNK_ITEMS), null);
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
});
