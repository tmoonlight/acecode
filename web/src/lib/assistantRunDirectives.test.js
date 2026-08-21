// 单元测试:buildAssistantRunDirectives 纯逻辑。
//
// 覆盖触发场景:
// - 连续 assistant + 中间穿插 tool 行: 只第一条带 header, 其余 continuation
// - 用户消息把 run 切断: 之后第一条 assistant 重新显示 header
// - 空内容 assistant: 隐藏整行 + 不消耗 header 名额
// - 当前 busy run 延迟 footer,已结束历史 run 不受影响
// - completion_summary 存在时独占 turn footer
// - 用户中断 / 错误消息在 busy 状态滞后时也成为唯一终态 footer
// - 合成终止项从同一 turn 最近的真实消息继承 fork messageId
// - 非数组 / null 输入: 返回空 Map

import assert from 'node:assert/strict';
import { buildAssistantRunDirectives } from './assistantRunDirectives.js';

function run(name, fn) {
  try {
    fn();
    console.log('ok - ' + name);
  } catch (err) {
    console.error('not ok - ' + name);
    throw err;
  }
}

run('非数组输入返回空 Map', () => {
  assert.equal(buildAssistantRunDirectives(null).size, 0);
  assert.equal(buildAssistantRunDirectives(undefined).size, 0);
  assert.equal(buildAssistantRunDirectives({}).size, 0);
});

run('单条 assistant 消息显示 header', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'hello' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: true });
});

run('连续 assistant + 中间 tool 行: 只第一条带 header', () => {
  // 模拟截图场景: assistant(空文本但 streaming 完成?) → tool → assistant → tool → assistant
  // 这里所有 assistant 都有内容, 只关心 header 合并
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '思考中...' },
    { kind: 'tool', id: 2, tool: { isDone: true } },
    { kind: 'msg', id: 3, role: 'assistant', content: '继续' },
    { kind: 'tool', id: 4, tool: { isDone: true } },
    { kind: 'msg', id: 5, role: 'assistant', content: '完成' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: false });
  assert.deepEqual(d.get(5), { showHeader: false, showFooter: true });
});

run('user 消息切断 run, 之后第一条 assistant 重新带 header', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '答一' },
    { kind: 'msg', id: 2, role: 'assistant', content: '答一-续' },
    { kind: 'msg', id: 3, role: 'user', content: '再问' },
    { kind: 'msg', id: 4, role: 'assistant', content: '答二' },
    { kind: 'msg', id: 5, role: 'assistant', content: '答二-续' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(2), { showHeader: false, showFooter: true });
  assert.deepEqual(d.get(4), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(5), { showHeader: false, showFooter: true });
});

run('空内容 + 非 streaming 的 assistant 隐藏 + 不消耗 header 名额', () => {
  // 触发场景: LLM 仅发起 tool_call 不发文本, daemon 仍落库一条空 assistant
  // 期望: 该条 hide, 头部名额留给后续真正有内容的 assistant
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '   ' },
    { kind: 'tool', id: 2, tool: { isDone: true } },
    { kind: 'msg', id: 3, role: 'assistant', content: '真正的输出' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { hide: true });
  assert.deepEqual(d.get(3), { showHeader: true, showFooter: true });
});

run('空内容 + streaming 的 assistant 隐藏 + 不消耗 header 名额', () => {
  // 触发场景: 旧 daemon / provider 空 token 生成了 streaming 空占位
  // 期望: 不显示静态时间戳, 头部名额留给后续真正有内容的 assistant
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: '', streaming: true },
    { kind: 'msg', id: 2, role: 'assistant', content: '后续' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { hide: true });
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: true });
});

run('多个 user 之间的多个 run, 各自独立计算', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'user', content: 'q1' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'a1' },
    { kind: 'msg', id: 3, role: 'assistant', content: 'a1-cont' },
    { kind: 'msg', id: 4, role: 'user', content: 'q2' },
    { kind: 'msg', id: 5, role: 'assistant', content: 'a2' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: true });
  assert.deepEqual(d.get(5), { showHeader: true, showFooter: true });
});

run('system 行不影响 run', () => {
  // 触发场景: 系统插入一条警告(role 既不是 user 也不是 assistant), 不应重置 run
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'a1' },
    { kind: 'msg', id: 2, role: 'system', content: '警告' },
    { kind: 'msg', id: 3, role: 'assistant', content: 'a2' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(3), { showHeader: false, showFooter: true });
});

run('activity summary 不切断 run,completion summary 独占 footer', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'assistant', content: 'first' },
    { kind: 'activity_summary', id: 's1', title: '调用 2 个工具' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'second' },
    { kind: 'completion_summary', id: 'c1', title: '总结：done' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(1), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get(2), { showHeader: false, showFooter: false });
  assert.deepEqual(d.get('c1'), { showFooter: true });
});

run('busy 只延迟当前最后一个 run,历史 turn footer 保持显示', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'user', content: 'q1' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'a1' },
    { kind: 'msg', id: 3, role: 'user', content: 'q2' },
    { kind: 'msg', id: 4, role: 'assistant', content: 'a2 streaming' },
  ];
  const d = buildAssistantRunDirectives(items, { deferLastFooter: true });
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: true });
  assert.deepEqual(d.get(4), { showHeader: true, showFooter: false });
});

run('busy turn 已出现 completion summary 时正文和总结都延迟 footer', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'user', content: 'do it' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'final text' },
    { kind: 'completion_summary', id: 'done', title: '总结：完成' },
  ];
  const d = buildAssistantRunDirectives(items, { deferLastFooter: true });
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get('done'), { showFooter: false });
});

run('同一 task_complete turn 结算后只有 completion summary 显示 footer', () => {
  const items = [
    { kind: 'msg', id: 1, role: 'user', content: 'do it' },
    { kind: 'msg', id: 2, role: 'assistant', content: 'final text' },
    { kind: 'completion_summary', id: 'done', title: '总结：完成' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get('done'), { showFooter: true });
});

run('用户中断立即让末端 termination notice 接管唯一 footer', () => {
  const items = [
    { kind: 'msg', id: 1, messageId: 'user-1', role: 'user', content: 'do it' },
    { kind: 'msg', id: 2, messageId: 'assistant-1', role: 'assistant', content: 'working' },
    { kind: 'termination_notice', id: 'aborted', content: '用户已终止本轮任务' },
  ];
  const d = buildAssistantRunDirectives(items, { deferLastFooter: true });
  assert.deepEqual(d.get(2), { showHeader: true, showFooter: false });
  assert.deepEqual(d.get('aborted'), {
    showFooter: true,
    forkMessageId: 'assistant-1',
  });
});

run('错误消息不用合成错误 id 分叉并覆盖滞后的 busy 状态', () => {
  const items = [
    { kind: 'msg', id: 1, messageId: 'user-2', role: 'user', content: 'do it' },
    { kind: 'msg', id: 'provider-error', messageId: 'synthetic-error-id', role: 'error', content: 'provider failed' },
  ];
  const d = buildAssistantRunDirectives(items, { deferLastFooter: true });
  assert.deepEqual(d.get('provider-error'), {
    showFooter: true,
    forkMessageId: 'user-2',
  });
});

run('终止提示继承折叠轨迹中最近的真实工具结果 id', () => {
  const items = [
    { kind: 'msg', id: 1, messageId: 'user-3', role: 'user', content: 'do it' },
    {
      kind: 'activity_summary',
      id: 'activity-1',
      collapsedItems: [
        { kind: 'tool', id: 2, messageId: 'tool-result-1', tool: { isDone: true } },
      ],
    },
    { kind: 'termination_notice', id: 'terminated', content: '任务已终止' },
  ];
  const d = buildAssistantRunDirectives(items);
  assert.deepEqual(d.get('terminated'), {
    showFooter: true,
    forkMessageId: 'tool-result-1',
  });
});

run('重新打开的历史终止 turn 保留 footer,新 busy turn 仍延迟', () => {
  const items = [
    { kind: 'msg', id: 1, messageId: 'user-old', role: 'user', content: 'old' },
    { kind: 'termination_notice', id: 'old-stop', content: '上次运行已终止' },
    { kind: 'msg', id: 2, messageId: 'user-new', role: 'user', content: 'new' },
    { kind: 'msg', id: 3, role: 'assistant', content: 'streaming' },
  ];
  const d = buildAssistantRunDirectives(items, { deferLastFooter: true });
  assert.deepEqual(d.get('old-stop'), {
    showFooter: true,
    forkMessageId: 'user-old',
  });
  assert.deepEqual(d.get(3), { showHeader: true, showFooter: false });
});
