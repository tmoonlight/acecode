import assert from 'node:assert/strict';
import {
  applyTranscriptReplayEvents,
  createTranscriptState,
  loadTranscriptHistory,
  orphanToolResultCallIds,
  reduceTranscriptEvent,
  repairOrphanToolRequests,
} from './sessionTranscript.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

// 实时工具伪行(daemon 的 tool_call / tool_result message 事件)与结构化工具行
// 的配对。回归:进行中的回合偶现一串只显示「Tool」、展开只有返回的工具行,
// 回合结束自愈后才恢复。

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function rowMeta(id, name, index = 0) {
  return { tool_call_id: id, tool_index: index, tool_name: name };
}

function persistedCall(id, name, args) {
  return { id, type: 'function', index: 0, function: { name, arguments: args } };
}

function projectLive(items) {
  return projectCollapsedTranscriptItems(items, { busy: true, live: true, deferTrailingToolSummary: true });
}

function projectedTools(items) {
  return projectLive(items)
    .flatMap((item) => item.collapsedItems || [item])
    .filter((item) => item.kind === 'tool');
}

function liveReducer() {
  let state = createTranscriptState({ isLive: true });
  let seq = 0;
  return {
    emit(type, payload) {
      state = reduceTranscriptEvent(state, { type, payload, seq: ++seq }).state;
    },
    get state() { return state; },
  };
}

function emitBashCall(live, id, command, output, { callId = 'call-row', resultId = 'result-row' } = {}) {
  live.emit('message', {
    role: 'tool_call', content: `[Tool: bash] {"command":"${command}"}`, is_tool: true,
    id: callId, metadata: rowMeta(id, 'bash'),
  });
  live.emit('tool_start', { tool: 'bash', tool_call_id: id, args: { command } });
  live.emit('tool_end', { tool: 'bash', tool_call_id: id, success: true, output });
  live.emit('message', {
    role: 'tool_result', content: output, is_tool: true, id: resultId, metadata: rowMeta(id, 'bash'),
  });
}

run('切进运行中会话:回放的工具伪行按 call id 与 REST 历史去重,不追加无名 Tool 行', () => {
  const history = loadTranscriptHistory(createTranscriptState({ isLive: true }), {
    messages: [
      { id: 'u1', role: 'user', content: 'start', ts: 1 },
      { id: 'a1', role: 'assistant', content: '', tool_calls: [persistedCall('c1', 'bash', '{"command":"git checkout -b feat"}')], ts: 2 },
      {
        id: 't1', role: 'tool', content: "Switched to a new branch 'feat'", tool_call_id: 'c1', ts: 3,
        metadata: { tool_summary: { icon: '*', verb: 'Ran', object: 'git checkout -b feat', metrics: [] } },
      },
    ],
    events: [],
    busy: true,
  }).state;

  let seq = 0;
  const replayed = (type, payload) => ({ type, payload, seq: ++seq, replayed: true });
  const state = applyTranscriptReplayEvents(history, [
    replayed('message', { role: 'tool_call', content: '[Tool: bash] {"command":"git checkout -b feat"}', is_tool: true, id: 'r1', metadata: rowMeta('c1', 'bash') }),
    replayed('tool_start', { tool: 'bash', tool_call_id: 'c1', args: { command: 'git checkout -b feat' } }),
    replayed('tool_end', { tool: 'bash', tool_call_id: 'c1', success: true, output: "Switched to a new branch 'feat'" }),
    replayed('message', { role: 'tool_result', content: "Switched to a new branch 'feat'", is_tool: true, id: 'r2', metadata: rowMeta('c1', 'bash') }),
    // 快照之后才开始的调用:命令与输出都和上一次一样,也必须照常出现。
    replayed('message', { role: 'tool_call', content: '[Tool: bash] {"command":"git checkout -b feat"}', is_tool: true, id: 'r3', metadata: rowMeta('c2', 'bash') }),
    replayed('tool_start', { tool: 'bash', tool_call_id: 'c2', args: { command: 'git checkout -b feat' } }),
    replayed('tool_end', { tool: 'bash', tool_call_id: 'c2', success: true, output: "Switched to a new branch 'feat'" }),
    replayed('message', { role: 'tool_result', content: "Switched to a new branch 'feat'", is_tool: true, id: 'r4', metadata: rowMeta('c2', 'bash') }),
  ]).state;

  const tools = projectedTools(state.items);
  assert.deepEqual(tools.map((item) => item.tool.toolCallId), ['c1', 'c2']);
  assert.deepEqual(tools.map((item) => item.tool.tool), ['bash', 'bash']);
  assert.equal(JSON.stringify(projectLive(state.items)).includes('请求未记录'), false);
});

run('WS 带游标重连:直接进 reducer 的回放伪行同样按 call id 判重', () => {
  let state = loadTranscriptHistory(createTranscriptState({ isLive: true }), {
    messages: [
      { id: 'u1', role: 'user', content: 'start', ts: 1 },
      { id: 'a1', role: 'assistant', content: '', tool_calls: [persistedCall('c1', 'bash', '{"command":"ls"}')], ts: 2 },
      { id: 't1', role: 'tool', content: 'README.md', tool_call_id: 'c1', ts: 3 },
    ],
    events: [],
    busy: true,
  }).state;
  const before = state.items.length;
  let seq = 0;
  for (const payload of [
    { role: 'tool_call', content: '[Tool: bash] {"command":"ls"}', is_tool: true, id: 'w1', metadata: rowMeta('c1', 'bash') },
    { role: 'tool_result', content: 'README.md', is_tool: true, id: 'w2', metadata: rowMeta('c1', 'bash') },
  ]) {
    state = reduceTranscriptEvent(state, { type: 'message', payload, seq: ++seq, replayed: true }).state;
  }
  assert.equal(state.items.length, before);
  assert.equal(state.lastSeq, 2);
  assert.equal(JSON.stringify(projectLive(state.items)).includes('请求未记录'), false);
});

run('实时回合:内容相同的两次调用不按消息 id 合并,结果各自归到自己的工具行', () => {
  const live = liveReducer();
  live.emit('message', { role: 'user', content: 'go', id: 'u1' });
  // 旧 daemon 的伪行 id 是内容哈希:两次 git status 的调用行 / 结果行 id 相同。
  emitBashCall(live, 'c1', 'git status', 'clean');
  emitBashCall(live, 'c2', 'git status', 'clean');
  for (const id of ['r1', 'r2', 'r3']) {
    live.emit('tool_start', { tool: 'file_read', tool_call_id: id, args: { file_path: `${id}.txt` } });
  }
  for (const id of ['r1', 'r2', 'r3']) {
    live.emit('message', { role: 'tool_call', content: `[Tool: file_read] {"file_path":"${id}.txt"}`, is_tool: true, id: `call-${id}`, metadata: rowMeta(id, 'file_read') });
    live.emit('tool_end', { tool: 'file_read', tool_call_id: id, success: true, output: 'same' });
    live.emit('message', { role: 'tool_result', content: 'same', is_tool: true, id: 'same-output', metadata: rowMeta(id, 'file_read') });
  }

  const rows = live.state.items.filter((item) => item.kind === 'msg' && item.role !== 'user');
  assert.equal(rows.length, 10);
  const tools = projectedTools(live.state.items);
  assert.deepEqual(tools.map((item) => item.tool.toolCallId), ['c1', 'c2', 'r1', 'r2', 'r3']);
  assert.equal(JSON.stringify(projectLive(live.state.items)).includes('请求未记录'), false);
  assert.deepEqual(orphanToolResultCallIds(live.state.items), []);
});

run('兜底:请求没送到的工具结果先显示工具名,再用 REST 尾页补回请求', () => {
  const live = liveReducer();
  live.emit('message', { role: 'user', content: 'go', id: 'u1' });
  emitBashCall(live, 'c1', 'git status', 'clean');
  // tool_start 与调用伪行在重连缺口里丢了,只剩结果伪行。
  live.emit('message', {
    role: 'tool_result', content: "Switched to a new branch 'feat'", is_tool: true, id: 'lost', metadata: rowMeta('c9', 'bash'),
  });

  assert.deepEqual(orphanToolResultCallIds(live.state.items), ['c9']);
  const orphan = projectedTools(live.state.items).find((item) => item.tool.toolCallId === 'c9');
  assert.equal(orphan.tool.tool, 'bash');
  assert.match(orphan.tool.output, /请求未记录/);

  const repaired = repairOrphanToolRequests(live.state, [
    { id: 'u1', role: 'user', content: 'go' },
    { id: 'a9', role: 'assistant', content: '', tool_calls: [persistedCall('c9', 'bash', '{"command":"git checkout -b feat"}')] },
  ]);
  assert.deepEqual(orphanToolResultCallIds(repaired.items), []);
  const restored = projectedTools(repaired.items).find((item) => item.tool.toolCallId === 'c9');
  assert.equal(restored.tool.tool, 'bash');
  assert.deepEqual(restored.tool.args, { command: 'git checkout -b feat' });
  assert.doesNotMatch(restored.tool.output, /请求未记录/);
  // 已配好的工具与转录其余部分保持原样。
  assert.equal(repaired.items.length, live.state.items.length + 1);
  assert.equal(repairOrphanToolRequests(repaired, []), repaired);
});

run('兜底只看当前回合:更早回合的旧记录不触发补拉', () => {
  const state = loadTranscriptHistory(createTranscriptState(), {
    messages: [
      { id: 'u1', role: 'user', content: 'old', ts: 1 },
      { id: 't1', role: 'tool', content: 'legacy', tool_call_id: 'legacy-call', ts: 2 },
      { id: 'u2', role: 'user', content: 'new', ts: 3 },
    ],
    events: [],
  }).state;
  assert.deepEqual(orphanToolResultCallIds(state.items), []);
});
