import assert from 'node:assert/strict';
import {
  NEXT_STEP_DISMISSED_STORAGE_KEY,
  activeNextStep,
  hasNextStepQuestion,
  nextStepKey,
  readDismissedNextSteps,
  withDismissedNextStep,
  wrapUpFromTool,
  writeDismissedNextSteps,
} from './nextStepBox.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function completion(id, wrapUp, messageId = `done-${id}`) {
  return { kind: 'completion_summary', id: `completion:${id}`, messageId, summary: 'done', wrapUp };
}

function memoryStorage(initial = {}) {
  const data = new Map(Object.entries(initial));
  return {
    getItem: (key) => (data.has(key) ? data.get(key) : null),
    setItem: (key, value) => data.set(key, String(value)),
    data,
  };
}

const askBox = { recap: '修好了', box: true, ask: '现在提交吗?', actions: ['提交', '先不提交'], blocked: false };

// 场景:daemon 在 tool metadata.wrap_up 里给出规范化结果(按钮可能是对象或字符串)。
// 期望:只在 box 为 true 时保留问题与按钮,按钮最多 2 个、去掉空文字;旧数据返回 null。
run('wrapUpFromTool 只在框显示时保留问题与按钮', () => {
  assert.deepEqual(wrapUpFromTool({
    metadata: { wrap_up: { recap: ' 修好了 ', box: true, ask: '提交吗?', actions: [{ label: '提交' }, '  ', 'B', 'C'], blocked: true } },
  }), { recap: '修好了', box: true, ask: '提交吗?', actions: ['提交', 'B'], blocked: true });
  assert.deepEqual(wrapUpFromTool({
    metadata: { wrap_up: { recap: 'done', ask: '没画框时不显示', actions: ['A'] } },
  }), { recap: 'done', box: false, ask: '', actions: [], blocked: false });
  assert.equal(wrapUpFromTool({ metadata: null }), null);
  assert.equal(wrapUpFromTool({ metadata: { wrap_up: [] } }), null);
});

// 场景:回合刚结束,transcript 最后一项是带问题的完成总结。
// 期望:框可操作;回合运行中、只读会话、已关闭、后面还有内容(用户已回复)时都不可操作。
run('activeNextStep 只认 transcript 末尾、回合已结束的带问题完成总结', () => {
  const items = [{ kind: 'msg', id: 1, role: 'user' }, completion(2, askBox)];
  assert.deepEqual(activeNextStep(items), {
    key: 'done-2', itemId: 'completion:2', ask: '现在提交吗?', actions: ['提交', '先不提交'], blocked: false,
  });
  assert.equal(activeNextStep(items, { busy: true }), null);
  assert.equal(activeNextStep(items, { interactive: false }), null);
  assert.equal(activeNextStep(items, { dismissed: { 'done-2': 1 } }), null);
  assert.equal(activeNextStep([...items, { kind: 'msg', id: 3, role: 'user' }]), null);
  assert.equal(activeNextStep([completion(4, { ...askBox, box: false, ask: '', actions: [] })]), null);
  assert.equal(activeNextStep([completion(5, null)]), null);
  assert.equal(activeNextStep([]), null);
});

// 场景:blocked 时模型只给了问题、没给按钮(用户在输入框回答)。
// 期望:仍算有问题、框可显示;既无问题也无按钮时不算。
run('hasNextStepQuestion 接受只有问题的 blocked 框', () => {
  assert.equal(hasNextStepQuestion(completion(1, { ...askBox, actions: [], blocked: true })), true);
  assert.equal(hasNextStepQuestion(completion(2, { ...askBox, ask: '', actions: [] })), false);
  assert.equal(hasNextStepQuestion({ kind: 'msg' }), false);
});

// 场景:完成总结缺 messageId(旧 daemon 的实时回合)。
// 期望:关闭记录退回用 tool_call_id,再退回投影 id,保证每个框都有稳定的键。
run('nextStepKey 依次取 messageId / tool_call_id / 投影 id', () => {
  assert.equal(nextStepKey({ messageId: 'm1', sourceItem: { tool: { toolCallId: 'c1' } }, id: 'x' }), 'm1');
  assert.equal(nextStepKey({ sourceItem: { tool: { toolCallId: 'c1' } }, id: 'x' }), 'c1');
  assert.equal(nextStepKey({ id: 'completion:9' }), 'completion:9');
});

// 场景:用户关闭过很多个框。
// 期望:关闭记录写入 localStorage 并能读回,只保留最近 200 条;存储坏掉或不可用时不抛错。
run('关闭记录持久化且有上限', () => {
  let dismissed = {};
  for (let i = 0; i < 205; i += 1) dismissed = withDismissedNextStep(dismissed, `k${i}`, i + 1);
  assert.equal(Object.keys(dismissed).length, 200);
  assert.equal(Object.prototype.hasOwnProperty.call(dismissed, 'k0'), false);
  assert.equal(dismissed.k204, 205);

  const storage = memoryStorage();
  writeDismissedNextSteps({ a: 1 }, storage);
  assert.deepEqual(readDismissedNextSteps(storage), { a: 1 });
  assert.deepEqual(readDismissedNextSteps(memoryStorage({ [NEXT_STEP_DISMISSED_STORAGE_KEY]: '{bad' })), {});
  assert.deepEqual(readDismissedNextSteps(memoryStorage({ [NEXT_STEP_DISMISSED_STORAGE_KEY]: '[1]' })), {});
  const throwing = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } };
  assert.deepEqual(readDismissedNextSteps(throwing), {});
  writeDismissedNextSteps({ a: 1 }, throwing);
  assert.deepEqual(withDismissedNextStep({ a: 1 }, ''), { a: 1 });
});
