import assert from 'node:assert/strict';
import {
  QUEUED_INPUT_STATE,
  cancelQueuedInput,
  completeQueuedInputForMessage,
  createChatInputQueueState,
  enqueueQueuedInput,
  markQueuedInputSending,
  nextQueuedInput,
  queuedInputsForSession,
} from './chatInputQueue.js';
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

const SID = 's-queue';

function texts(state) {
  return queuedInputsForSession(state, SID).map((item) => item.content);
}

// 回归见证(bug 表现:busy 期间排队的消息偶尔"没了",或已经取消的又冒出来)。
// 旧实现里排队状态有两个写入者:updateQueueState 命令式写 queueStateRef,一个
// 被动 effect 又把某次渲染保存的旧快照写回同一个引用。被动 effect 在提交后异步
// 刷新,一旦回写落在"提交之后、下一次提交之前",这中间的排队变更就被吞掉。
// 这个用例先按同样时序驱动旧模型证明它确实丢消息,再证明 store 模型不会。
run('渲染快照回写会吞掉排队消息,store 模型不会', () => {
  // --- 旧模型:引用同时被写入者与渲染快照写 ---
  let ref = createChatInputQueueState();
  ref = enqueueQueuedInput(ref, { sessionId: SID, text: '第一条', now: 100 });
  const renderedSnapshot = ref; // React 提交时保存的快照
  ref = enqueueQueuedInput(ref, { sessionId: SID, text: '第二条', now: 101 });
  ref = renderedSnapshot; // 迟到的被动 effect 回写
  ref = enqueueQueuedInput(ref, { sessionId: SID, text: '第三条', now: 102 });

  assert.deepEqual(texts(ref), ['第一条', '第三条'], '旧模型应当丢掉中间那条');

  // --- 新模型:同样时序,渲染侧只能读 ---
  const store = createSingleWriterStore(createChatInputQueueState());
  const rendered = [];
  store.subscribe(() => { rendered.push(store.getState()); });

  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '第一条', now: 100 }));
  const staleSnapshot = rendered[rendered.length - 1];
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '第二条', now: 101 }));
  // 唯一写入口只接受 producer,值形式的回写在入口就被拒绝。
  assert.throws(() => store.commit(staleSnapshot), TypeError);
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '第三条', now: 102 }));

  assert.deepEqual(texts(store.getState()), ['第一条', '第二条', '第三条']);
});

// 触发场景:用户点了"取消",紧接着 drain 被触发(旧模型下取消可能被一份过期
// 快照抹掉,卡片重新出现并被真的发出去)。
// 期望行为:drain 的取件基于最新状态,已取消的那条取不到,取消不会被复活。
run('取消后 drain 取不到这条,取消不会被复活', () => {
  const store = createSingleWriterStore(createChatInputQueueState());
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '要取消的', now: 100 }));
  const [queued] = queuedInputsForSession(store.getState(), SID);

  const staleSnapshot = store.getState();
  store.commit((prev) => cancelQueuedInput(prev, queued.queued.id));
  assert.deepEqual(texts(store.getState()), []);

  assert.throws(() => store.commit(staleSnapshot), TypeError);

  let picked = null;
  store.commit((prev) => {
    picked = nextQueuedInput(prev, SID);
    return picked ? markQueuedInputSending(prev, picked.queued.id) : prev;
  });
  assert.equal(picked, null);
  assert.deepEqual(texts(store.getState()), []);
});

// 触发场景:drain 取出待发送项并标记 sending。ChatView 把这两步放进同一次提交,
// 所以"取件"看到的一定是提交时的最新状态。
// 期望行为:取出的就是队首,提交后它立刻变成 SENDING,不会被重复取第二次。
run('取件与标记 sending 在同一次提交内完成', () => {
  const store = createSingleWriterStore(createChatInputQueueState());
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '甲', now: 100 }));
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '乙', now: 101 }));

  let picked = null;
  store.commit((prev) => {
    picked = nextQueuedInput(prev, SID);
    return picked ? markQueuedInputSending(prev, picked.queued.id) : prev;
  });

  assert.equal(picked.content, '甲');
  assert.equal(
    queuedInputsForSession(store.getState(), SID)[0].queued.state,
    QUEUED_INPUT_STATE.SENDING,
  );
  // 队首已是 SENDING,下一次取件不会再取到它。
  const again = nextQueuedInput(store.getState(), SID);
  assert.equal(again, null);
});

// 触发场景:一轮 transcript 里出现多条已落库的 user 消息,需要逐条把对应排队项
// 标记完成。ChatView 把整轮遍历放进一次提交。
// 期望行为:遍历基于同一份状态逐步推进,期间新入队的消息不会被过期快照覆盖。
run('整轮完成标记在一次提交内遍历,不覆盖期间的新入队', () => {
  const store = createSingleWriterStore(createChatInputQueueState());
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '甲', now: 100 }));
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '乙', now: 101 }));
  // 只有已发出(SENDING)的排队项才会被落库的 user 消息标记完成。
  const sentIds = queuedInputsForSession(store.getState(), SID).map((item) => item.queued.id);
  store.commit((prev) => sentIds.reduce(
    (acc, id) => markQueuedInputSending(acc, id, { now: 200 }),
    prev,
  ));

  const persistedUserMessages = [
    { kind: 'msg', role: 'user', content: '甲', ts: 300 },
    { kind: 'msg', role: 'user', content: '乙', ts: 301 },
  ];

  store.commit((prev) => {
    let next = prev;
    for (const item of persistedUserMessages) {
      next = completeQueuedInputForMessage(next, {
        sessionId: SID,
        content: item.content,
        ts: item.ts,
      });
    }
    return next;
  });

  assert.deepEqual(texts(store.getState()), []);

  // 提交之后再入队仍然正常可见 —— 不会被上一次提交的输入快照回退。
  store.commit((prev) => enqueueQueuedInput(prev, { sessionId: SID, text: '丙', now: 102 }));
  assert.deepEqual(texts(store.getState()), ['丙']);
});
