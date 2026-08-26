import assert from 'node:assert/strict';
import {
  createTranscriptState,
  lastAssistantText,
  loadTranscriptHistory,
  preserveLiveAssistantTailOnLoad,
  reduceTranscriptEvent,
} from './sessionTranscript.js';
import { createSingleWriterStore } from './singleWriterStore.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 现场形状:一次 355 字的回复被服务端切成 244 个文本增量事件,序号连续、内容
// 完整;绝大多数增量只有 1~2 个字符,并且含 Markdown 标记与换行 —— 丢掉其中
// 任意一个反引号、星号或换行,后面一整块的结构和样式都会变形。
const REPLY = [
  '# 旧书店\n\n',
  '这家店开在**巷子最深处**,门脸只有一扇窄窗。\n\n',
  '- 一层是旧平装书,按 `出版年份` 排;\n',
  '- 二层堆着地图册和过期杂志;\n',
  '- 三层不对外开放。\n\n',
  '老板说:「书自己会找读者。」\n\n',
  '```text\n营业时间 10:00-22:00\n```\n',
  '雨天的时候,整条巷子只剩下翻书声。',
].join('');

// 按真实形状切成 244 个增量:大部分 1~2 字符。
function splitIntoChunks(text, count) {
  const chunks = [];
  const size = Math.max(1, Math.floor(text.length / count));
  for (let i = 0; i < text.length; i += size) {
    chunks.push(text.slice(i, i + size));
  }
  return chunks;
}

const CHUNKS = splitIntoChunks(REPLY, 244);

function tokenEvents(chunks, startSeq = 1) {
  return chunks.map((text, index) => ({
    type: 'token',
    seq: startSeq + index,
    payload: { text },
  }));
}

function assistantText(state) {
  const item = [...(state.items || [])]
    .reverse()
    .find((entry) => entry.kind === 'msg' && entry.role === 'assistant');
  return item ? (item.content || '') : '';
}

function initialState() {
  return createTranscriptState({ isLive: true, loadState: 'loaded' });
}

// 触发场景:一次回复的全部文本增量以高速率顺序到达,期间 React 不断提交渲染。
// 期望行为:任意时刻已呈现的流式正文都是最终正文的严格前缀,收尾时完全一致。
// 这是本次修复的核心不变量 —— 正常流式输出永远只是最终答案的前缀,不应该
// 中间随机缺块。
run('高速流式重放:任意时刻流式正文都是最终正文的前缀', () => {
  const store = createSingleWriterStore(initialState());
  const events = tokenEvents(CHUNKS);

  // React 侧只订阅:拿到通知后重新读取快照,没有任何写回入口。
  const renderedSnapshots = [];
  store.subscribe(() => { renderedSnapshots.push(store.getState()); });

  events.forEach((event) => {
    store.commit((prev) => reduceTranscriptEvent(prev, event).state);
    const shown = assistantText(store.getState());
    assert.equal(REPLY.startsWith(shown), true, `流式正文不是最终正文的前缀: ${JSON.stringify(shown.slice(-40))}`);
  });

  assert.equal(assistantText(store.getState()), REPLY);
  assert.equal(lastAssistantText(store.getState()), REPLY);
  assert.equal(renderedSnapshots.length, events.length);
  assert.equal(store.getRevision(), events.length);
});

// 回归见证(bug 表现:长会话喷字时正文中间随机缺汉字、标点和换行,后面的文字
// 却已经出现;回合结束后被完整消息覆盖回正确结果)。
// 旧实现里 React 的被动 effect 会把某次渲染保存的旧快照写回权威状态引用。这个
// 用例先按同样时序驱动旧模型,证明它确实产出非前缀正文(即这条时序是真实的
// 失效路径,不是臆测);再证明 store 的写入口根本表达不出这次回写。
run('旧的渲染快照回写时序会丢片段,store 的写入口拒绝该时序', () => {
  const events = tokenEvents(CHUNKS);
  // 回写延迟 = 3 个事件:模拟"提交之后、下一条 WS 消息之前"才刷新的被动 effect。
  const WRITE_BACK_DELAY = 3;

  // --- 旧模型:stateRef 同时被事件与渲染快照写入 ---
  let staleRef = initialState();
  const snapshots = [];
  events.forEach((event, index) => {
    if (index >= WRITE_BACK_DELAY) staleRef = snapshots[index - WRITE_BACK_DELAY];
    staleRef = reduceTranscriptEvent(staleRef, event).state;
    snapshots[index] = staleRef;
  });
  const damaged = assistantText(staleRef);
  assert.notEqual(damaged, REPLY);
  assert.equal(REPLY.startsWith(damaged), false, '旧模型应当产出非前缀正文');
  // 传输层完全看不出异常:序号仍然推进到最后一个事件。
  assert.equal(staleRef.lastSeq, events[events.length - 1].seq);

  // --- 新模型:同样时序,渲染侧只能读 ---
  const store = createSingleWriterStore(initialState());
  const rendered = [];
  store.subscribe(() => { rendered.push(store.getState()); });
  events.forEach((event, index) => {
    if (index >= WRITE_BACK_DELAY) {
      const staleSnapshot = rendered[index - WRITE_BACK_DELAY];
      // 唯一写入口只接受 producer,值形式的回写在入口就被拒绝。
      assert.throws(() => store.commit(staleSnapshot), TypeError);
    }
    store.commit((prev) => reduceTranscriptEvent(prev, event).state);
    assert.equal(REPLY.startsWith(assistantText(store.getState())), true);
  });
  assert.equal(assistantText(store.getState()), REPLY);
});

// 触发场景:WebSocket 重复投递或迟到的低序号事件在流式过程中到达。
// 期望行为:序号不大于已应用序号的事件被忽略,流式正文保持不变、不重复拼接。
run('重复与乱序事件在流式过程中保持幂等', () => {
  const store = createSingleWriterStore(initialState());
  const events = tokenEvents(CHUNKS.slice(0, 20));
  events.forEach((event) => {
    store.commit((prev) => reduceTranscriptEvent(prev, event).state);
  });

  const expected = CHUNKS.slice(0, 20).join('');
  assert.equal(assistantText(store.getState()), expected);

  const revisionBefore = store.getRevision();
  store.commit((prev) => reduceTranscriptEvent(prev, events[19]).state);
  store.commit((prev) => reduceTranscriptEvent(prev, events[5]).state);

  assert.equal(assistantText(store.getState()), expected);
  // reducer 对过期事件返回同一对象,store 因此不升版本、不触发多余渲染。
  assert.equal(store.getRevision(), revisionBefore);
});

// 触发场景:历史加载的 REST 快照在流式进行中返回,而这份快照比实时状态更旧。
// 期望行为:读取当前状态、与实时尾巴合并、写回在同一次提交内完成,已收到的
// 流式尾巴不被这份更旧的快照截断。
run('历史加载与流式并发时在单次提交内合并,不截断实时尾巴', () => {
  const store = createSingleWriterStore(initialState());
  tokenEvents(CHUNKS).forEach((event) => {
    store.commit((prev) => reduceTranscriptEvent(prev, event).state);
  });

  // REST 快照落后:只含较早的一段 assistant 正文。
  const staleHistory = {
    messages: [
      { id: 'u-1', role: 'user', content: '讲讲那家旧书店' },
      { id: 'a-1', role: 'assistant', content: REPLY.slice(0, 40) },
    ],
  };

  store.commit((prev) => {
    const loaded = loadTranscriptHistory(prev, staleHistory);
    return preserveLiveAssistantTailOnLoad(loaded.state, prev);
  });

  assert.equal(assistantText(store.getState()), REPLY);
});

// 触发场景:用户在流式过程中切到另一个会话。
// 期望行为:store 提交一份属于新会话的初始状态,旧会话残留内容不再出现;
// 新会话的事件从空状态开始拼接。
run('会话切换提交新会话初始状态,不残留上一会话内容', () => {
  const store = createSingleWriterStore(initialState());
  tokenEvents(CHUNKS.slice(0, 30)).forEach((event) => {
    store.commit((prev) => reduceTranscriptEvent(prev, event).state);
  });
  assert.notEqual(assistantText(store.getState()), '');

  store.commit(() => createTranscriptState({ isLive: true, loadState: 'loading' }));
  assert.equal(store.getState().items.length, 0);
  assert.equal(store.getState().lastSeq, 0);

  store.commit((prev) => reduceTranscriptEvent(prev, {
    type: 'token',
    seq: 1,
    payload: { text: '新会话第一句' },
  }).state);
  assert.equal(assistantText(store.getState()), '新会话第一句');
});
