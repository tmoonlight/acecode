// structuralState 纯逻辑测试。
//
// 背景:侧栏每 5 秒刷新,数据没变也写回新的 Map / Set / 数组,导致每轮重新渲染;
// 侧栏的 useCallback 闭包把每次渲染的作用域串成链,Desktop 挂机一两天后渲染进程
// 内存耗尽。这里守住「内容相同就不写入」的判定,保证空闲刷新既不渲染也不留 update。

import assert from 'node:assert/strict';
import vm from 'node:vm';
import {
  commitIfChanged,
  keepIfStructurallyEqual,
  structurallyEqual,
} from './structuralState.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:一轮刷新拿回与上一轮完全相同的会话列表(新数组、新对象、含嵌套数组)。
// 期望:判为相同,沿用旧数组 —— React 因此跳过渲染。
run('structurallyEqual 会话列表内容相同即相同', () => {
  const prev = [
    { id: 's1', title: 'A', busy: false, todos: [{ text: 'x', done: false }], extra: null },
    { id: 's2', title: 'B', busy: true, timestamp_ms: 10 },
  ];
  const next = prev.map((item) => JSON.parse(JSON.stringify(item)));
  assert.equal(structurallyEqual(prev, next), true);
  assert.equal(keepIfStructurallyEqual(prev, next), prev);
});

// 场景:会话状态 Map / 已加载工作区 Set / 置顶表(Map<string, string[]>)每轮都新建。
// 期望:键值相同即相同;Set 成员相同即相同,不看插入顺序。
run('structurallyEqual 支持 Map 与 Set', () => {
  const prevStatus = new Map([['s1', { state: 'read', cursor: 3 }], ['s2', { state: 'unread', cursor: 5 }]]);
  const nextStatus = new Map([['s1', { state: 'read', cursor: 3 }], ['s2', { state: 'unread', cursor: 5 }]]);
  assert.equal(structurallyEqual(prevStatus, nextStatus), true);
  assert.equal(structurallyEqual(new Set(['a', 'b']), new Set(['b', 'a'])), true);
  assert.equal(structurallyEqual(new Map([['w', ['s1', 's2']]]), new Map([['w', ['s1', 's2']]])), true);
});

// 场景:真实变化 —— 某个会话变成忙碌、标题改了、多了一个会话、置顶顺序变了、
// 键缺失但值为 undefined。期望:都判为不同,照常更新。
run('structurallyEqual 识别真实变化', () => {
  assert.equal(structurallyEqual([{ id: 's1', busy: false }], [{ id: 's1', busy: true }]), false);
  assert.equal(structurallyEqual([{ id: 's1' }], [{ id: 's1' }, { id: 's2' }]), false);
  assert.equal(structurallyEqual(new Map([['w', ['a', 'b']]]), new Map([['w', ['b', 'a']]])), false);
  assert.equal(structurallyEqual(new Map([['s1', 1]]), new Map([['s2', 1]])), false);
  assert.equal(structurallyEqual(new Set(['a']), new Set(['b'])), false);
  assert.equal(structurallyEqual({ a: undefined }, { b: undefined }), false);
  assert.equal(structurallyEqual([1], { 0: 1, length: 1 }), false);
  assert.equal(structurallyEqual(new Map(), new Set()), false);
});

// 场景:状态里出现 Date / 类实例 / 函数。期望:只认同一引用,不做猜测。
run('structurallyEqual 非普通对象只认同一引用', () => {
  const fn = () => {};
  assert.equal(structurallyEqual({ fn }, { fn }), true);
  assert.equal(structurallyEqual({ fn }, { fn: () => {} }), false);
  assert.equal(structurallyEqual(new Date(1), new Date(1)), false);
  assert.equal(structurallyEqual(NaN, NaN), true);
});

// 场景:对象来自另一个 realm(iframe、vm 上下文),原型不是本 realm 的 Object.prototype。
// 期望:内容相同仍判为相同;类实例仍只认同一引用。
run('structurallyEqual 跨 realm 的普通对象按内容比较', () => {
  const foreign = vm.runInNewContext('({ id: "s1", tags: ["a"], meta: { n: 1 } })');
  assert.equal(structurallyEqual({ id: 's1', tags: ['a'], meta: { n: 1 } }, foreign), true);
  class Row { constructor() { this.id = 's1'; } }
  assert.equal(structurallyEqual(new Row(), new Row()), false);
});

// 场景:每轮刷新都用内容相同的新数组调用 setter(函数式更新或直接值)。
// 期望:完全不调用 setState —— 只在 updater 里返回旧值不够,React 18 会把这类 update
// 挂在 hook 队列里直到组件下次渲染,组件空闲时队列会无限增长(修复过程中实测
// 打开会话的页面每分钟多出约 0.24 MB)。内容变化时才写入,且 ref 同步成新值。
run('commitIfChanged 内容相同不调用 setState', () => {
  const ref = { current: [{ id: 's1' }] };
  const writes = [];
  const setState = (value) => { writes.push(value); };
  const before = ref.current;
  assert.equal(commitIfChanged(ref, setState, (prev) => prev.map((item) => ({ ...item }))), before);
  assert.equal(commitIfChanged(ref, setState, [{ id: 's1' }]), before);
  assert.equal(writes.length, 0);
  assert.equal(ref.current, before);

  const next = commitIfChanged(ref, setState, (prev) => [...prev, { id: 's2' }]);
  assert.equal(writes.length, 1);
  assert.equal(writes[0], next);
  assert.equal(ref.current, next);
  assert.deepEqual(next, [{ id: 's1' }, { id: 's2' }]);
});

// 场景:同一轮里连续两次更新(例如先写单个工作区的会话,再写合并后的列表)。
// 期望:第二次的 updater 拿到第一次的结果,而不是渲染前的旧值。
run('commitIfChanged 连续更新以最新值为基准', () => {
  const ref = { current: new Set(['w1']) };
  const writes = [];
  commitIfChanged(ref, (value) => writes.push(value), (prev) => new Set(prev).add('w2'));
  commitIfChanged(ref, (value) => writes.push(value), (prev) => new Set(prev).add('w3'));
  assert.equal(writes.length, 2);
  assert.deepEqual([...ref.current], ['w1', 'w2', 'w3']);
});
