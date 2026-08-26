import assert from 'node:assert/strict';
import { createTranscriptStore } from './transcriptStore.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:连续多次提交发生在同一轮事件循环里(WebSocket 高速下发 token)。
// 期望行为:每个 producer 都读到上一次提交的结果,提交顺序与调用顺序一致。
run('每次提交的 producer 都读到最新已提交状态', () => {
  const store = createTranscriptStore({ text: '' });
  const seen = [];

  store.commit((prev) => { seen.push(prev.text); return { text: `${prev.text}A` }; });
  store.commit((prev) => { seen.push(prev.text); return { text: `${prev.text}B` }; });
  store.commit((prev) => { seen.push(prev.text); return { text: `${prev.text}C` }; });

  assert.deepEqual(seen, ['', 'A', 'AB']);
  assert.equal(store.getState().text, 'ABC');
  assert.equal(store.getRevision(), 3);
});

// 触发场景:reducer 判定事件为重复/过期,返回同一个 state 对象。
// 期望行为:不升版本、不通知订阅者,避免无意义的重渲染。
run('producer 返回同一对象或空值时不产生新版本', () => {
  const initial = { text: 'A' };
  const store = createTranscriptStore(initial);
  let notified = 0;
  store.subscribe(() => { notified += 1; });

  assert.equal(store.commit((prev) => prev), initial);
  assert.equal(store.commit(() => null), initial);
  assert.equal(store.commit(() => undefined), initial);

  assert.equal(store.getRevision(), 0);
  assert.equal(notified, 0);
  assert.equal(store.getState(), initial);
});

// 回归:旧实现把“某次渲染保存的旧状态”当作值写回状态引用,吞掉了这之后已到达
// 的 token(长会话喷字时正文中间随机缺字)。store 的写入口只接受 producer,
// 值形式的写入在入口就被拒绝,那条时序无法再被表达出来。
run('commit 拒绝值形式的写入', () => {
  const store = createTranscriptStore({ text: 'A' });
  const staleSnapshot = store.getState();
  store.commit((prev) => ({ text: `${prev.text}B` }));

  assert.throws(() => store.commit(staleSnapshot), TypeError);
  assert.throws(() => store.commit({ text: 'X' }), TypeError);
  assert.throws(() => store.commit(null), TypeError);
  assert.equal(store.getState().text, 'AB');
});

// 触发场景:订阅者在收到通知时又提交一次(例如自愈覆写最近一轮)。
// 期望行为:嵌套 producer 仍读到最新状态,通知不丢失,也不递归重复广播同一份。
run('订阅者内部再次提交时顺序与通知都保持正确', () => {
  const store = createTranscriptStore({ text: '' });
  const observed = [];
  let nested = false;

  store.subscribe(() => {
    observed.push(store.getState().text);
    if (!nested) {
      nested = true;
      store.commit((prev) => ({ text: `${prev.text}!` }));
    }
  });

  store.commit((prev) => ({ text: `${prev.text}A` }));

  assert.equal(store.getState().text, 'A!');
  // 第一次通知看到 A;嵌套提交在通知循环里置位,外层循环再广播一次 A!。
  assert.deepEqual(observed, ['A', 'A!']);
  assert.equal(store.getRevision(), 2);
});

// 触发场景:组件卸载 / 会话切换时退订。
// 期望行为:退订后不再收到通知,其余订阅者不受影响。
run('订阅与退订相互独立', () => {
  const store = createTranscriptStore({ text: '' });
  let a = 0;
  let b = 0;
  const offA = store.subscribe(() => { a += 1; });
  store.subscribe(() => { b += 1; });

  store.commit((prev) => ({ text: `${prev.text}1` }));
  offA();
  store.commit((prev) => ({ text: `${prev.text}2` }));

  assert.equal(a, 1);
  assert.equal(b, 2);
  // 通知不携带状态:订阅者只能重新读取,手里不会留下一份可以拿去回写的旧状态。
  assert.equal(store.getState().text, '12');
});

// 触发场景:传入非函数订阅者(防御性调用)。
// 期望行为:静默忽略并返回一个可安全调用的退订函数,不抛错。
run('非函数订阅者被安全忽略', () => {
  const store = createTranscriptStore({ text: '' });
  const off = store.subscribe(null);
  assert.equal(typeof off, 'function');
  off();
  store.commit((prev) => ({ text: `${prev.text}A` }));
  assert.equal(store.getState().text, 'A');
});
