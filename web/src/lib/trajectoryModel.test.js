import assert from 'node:assert/strict';
import { deriveTrajectoryLayout } from '../components/trajectory/deepseek/layout.ts';

import {
  buildDeepSeekTrajectory,
  mergeTrajectoryRecords,
  resolveDeepSeekTrajectoryPartial,
} from './trajectoryModel.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const recorded = (sequence, timestamp, type, payload = {}) => ({
  schema_version: 1,
  sequence,
  timestamp_ms: timestamp,
  type,
  source: 'recorded',
  payload,
});

function liveTurnRecords(count = 4) {
  const records = [{
    sequence: null, legacy_index: 0, type: 'legacy_context',
    payload: { role: 'user', is_meta: true, content: 'request context' },
  }];
  const append = (type, payload) => records.push(recorded(records.length, 1000 + records.length, type, payload));
  for (let turn = 1; turn <= count; turn += 1) {
    const id = `user-${turn}`;
    // The daemon writes the user Message before start_user_turn records turn_start.
    append('message', { role: 'user', id, content: `input ${turn}` });
    append('turn_start', { turn_id: id, user_message_id: id });
    append('model_step_start', { step_index: 1 });
    append('model_request', { step_index: 1, messages: [{ role: 'system', content: `prompt ${turn}` }] });
    append('model_response', { step_index: 1, content: `answer ${turn}` });
    append('turn_end', { turn_id: id });
  }
  return records;
}

run('real daemon ordering counts context, input and turn_start as one turn', () => {
  const records = liveTurnRecords();
  const projection = buildDeepSeekTrajectory(records);
  assert.deepEqual(projection.turns.map((turn) => turn.turn), [1, 2, 3, 4]);
  assert.deepEqual(projection.requests.map((request) => request.turn), [1, 2, 3, 4]);
  for (const turn of projection.turns) {
    const cells = turn.groups.flatMap((group) => group.cells);
    assert.equal(cells.find((cell) => cell.kind === 'user').previewMarkdown, `input ${turn.turn}`);
    assert.equal(cells.find((cell) => cell.kind === 'message').outputDetail, `answer ${turn.turn}`);
  }
});

run('every incremental prefix keeps inputs in their own turn before assistant output', () => {
  const records = liveTurnRecords();
  let merged = [];
  for (const record of records) {
    merged = mergeTrajectoryRecords(merged, [record]);
    const projection = buildDeepSeekTrajectory(merged);
    const turnNumbers = projection.turns.map((turn) => turn.turn);
    assert.deepEqual(turnNumbers, [...turnNumbers].sort((a, b) => a - b));
    for (const turn of projection.turns) {
      const inputs = turn.groups.flatMap((group) => group.cells).filter((cell) => cell.kind === 'user');
      for (const input of inputs) assert.equal(input.previewMarkdown, `input ${turn.turn}`);
    }
    const userCount = merged.filter((r) => r.type === 'message' && r.payload.role === 'user').length;
    assert.equal(projection.latestTurnNumber, Math.max(1, userCount));
  }
});

run('user-first lifecycle aliases and repeated start markers reuse the input turn', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1, 'message', { id: 'user-id', role: 'user', content: 'input' }),
    recorded(2, 2, 'turn_start', { turn_id: 'turn-id', user_message_id: 'user-id' }),
    recorded(3, 3, 'turn_start', { turn_id: 'turn-id', user_message_id: 'user-id' }),
    recorded(4, 4, 'model_request', { step_index: 1 }),
    recorded(5, 5, 'turn_end', { turn_id: 'turn-id' }),
    recorded(6, 6, 'turn_end', { turn_id: 'unknown-tail-boundary' }),
  ]);
  assert.equal(projection.latestTurnNumber, 1);
  assert.deepEqual(projection.turns.map((turn) => turn.turn), [1]);
});

run('inputs from a turn without an assistant never move to a later reply', () => {
  const records = liveTurnRecords(2).filter((record) => !(record.type === 'model_response' && record.payload.content === 'answer 1'));
  const projection = buildDeepSeekTrajectory(records);
  assert.deepEqual(projection.turns.map((turn) => turn.turn), [1, 2]);
  for (const turn of projection.turns) {
    const user = turn.groups.flatMap((group) => group.cells).find((cell) => cell.kind === 'user');
    assert.equal(user.previewMarkdown, `input ${turn.turn}`);
  }
});

run('live identity overrides an outdated ordinal before and after trajectory polling', () => {
  const all = liveTurnRecords(7);
  const previous = all.slice(0, -6);
  const live = { turnId: 'user-7', turn: 2, step: 1, blocks: [{ kind: 'text', text: 'stream seven' }] };
  for (let count = previous.length; count < all.length - 1; count += 1) {
    const projection = buildDeepSeekTrajectory(all.slice(0, count));
    const partial = resolveDeepSeekTrajectoryPartial(projection, live);
    assert.equal(partial.turn, 7);
    const turns = deriveTrajectoryLayout({ ...projection, partial });
    assert.deepEqual(turns.map((turn) => turn.turn), [1, 2, 3, 4, 5, 6, 7]);
    assert.equal(turns.at(-1).groups.at(-1).cells.at(-1).outputDetail, 'stream seven');
  }
  assert.equal(resolveDeepSeekTrajectoryPartial(buildDeepSeekTrajectory(all.slice(0, -1)), live), null);
  assert.equal(resolveDeepSeekTrajectoryPartial(buildDeepSeekTrajectory(all), live), null);
});

run('failed durable request suppresses a lingering partial even without assistant content', () => {
  const records = liveTurnRecords(1);
  records.find((record) => record.type === 'model_response').payload = { step_index: 1, status: 'error', content: '' };
  assert.equal(resolveDeepSeekTrajectoryPartial(buildDeepSeekTrajectory(records), {
    turnId: 'user-1', turn: 1, step: 1, blocks: [{ kind: 'text', text: 'old partial' }],
  }), null);
});

run('initial context and a tail page without lifecycle identity do not push live output into a phantom turn', () => {
  const all = liveTurnRecords(1);
  const live = { turnId: 'user-1', turn: 12, step: 1, blocks: [{ kind: 'text', text: 'live' }] };
  for (const records of [[], all.slice(0, 1), all.slice(0, 2), all.slice(0, 5), all.slice(3, 5)]) {
    const projection = buildDeepSeekTrajectory(records);
    const partial = resolveDeepSeekTrajectoryPartial(projection, live);
    assert.equal(partial.turn, 1);
    assert.deepEqual(deriveTrajectoryLayout({ ...projection, partial }).map((turn) => turn.turn), [1]);
  }
});

run('steering and synthetic tool input stay in the current user turn', () => {
  const all = liveTurnRecords(1);
  all.splice(5, 0,
    recorded(4.1, 5, 'message', { role: 'user', id: 'steer', content: 'steering', metadata: { turn_steer: true, turn_id: 'user-1' } }),
    recorded(4.2, 6, 'message', { role: 'user', id: 'tool-input', content: 'tool input', metadata: { synthetic_user_prompt: true } }),
  );
  const projection = buildDeepSeekTrajectory(all);
  assert.deepEqual(projection.turns.map((turn) => turn.turn), [1]);
  assert.equal(projection.turns[0].groups.flatMap((group) => group.cells).filter((cell) => cell.kind === 'user').length, 3);
});

run('ACE records project into DeepSeek Message and Step groups', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1', started_at_ms: 1000 }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'inspect repo' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', {
      step_index: 1,
      provider: 'openai',
      model: 'gpt-test',
      context_window: 128000,
      messages: [
        { role: 'system', content: 'system prompt' },
        { role: 'user', content: 'inspect repo' },
      ],
      tools: [{
        name: 'read',
        native_name: 'file_read',
        description: 'Read a file',
        parameters: { type: 'object' },
      }],
    }),
    recorded(5, 1040, 'model_first_output', { step_index: 1, channel: 'reasoning' }),
    recorded(6, 1100, 'model_response', {
      step_index: 1,
      status: 'completed',
      content: 'calling a tool',
      reasoning_content: 'need a file',
      usage: {
        prompt_tokens: 40,
        cache_read_tokens: 10,
        completion_tokens: 8,
        reasoning_tokens: 3,
      },
      tool_calls: [{ id: 'call-1', name: 'file_read', arguments: '{"file_path":"a"}' }],
    }),
    recorded(7, 1101, 'model_step_finish', { step_index: 1, reason: 'tool_calls' }),
    recorded(8, 1110, 'tool_start', {
      tool: 'file_read',
      tool_call_id: 'call-1',
      args: { file_path: 'a' },
      started_at_ms: 1110,
    }),
    recorded(9, 1160, 'tool_end', {
      tool: 'file_read',
      tool_call_id: 'call-1',
      success: true,
      output: 'contents',
      started_at_ms: 1110,
      completed_at_ms: 1160,
      duration_ms: 50,
    }),
    recorded(10, 1200, 'turn_end', { turn_id: 'turn-1', outcome: 'completed' }),
  ]);

  assert.equal(projection.turns.length, 1);
  assert.deepEqual(projection.turns[0].groups.map((group) => group.title), ['Message', 'Step 1']);
  const messageCells = projection.turns[0].groups[0].cells;
  assert.deepEqual(messageCells.map((cell) => cell.kind), ['system', 'user']);
  assert.equal(messageCells[0].text, 'Initial System Prompt');
  assert.equal(messageCells[0].promptDetail.system, 'system prompt');
  assert.equal(messageCells[0].promptDetail.tools[0].name, 'file_read');

  const [assistant, tool] = projection.turns[0].groups[1].cells;
  assert.equal(assistant.kind, 'message');
  assert.equal(assistant.outputDetail, 'calling a tool');
  assert.equal(assistant.thinkingDetail, 'need a file');
  assert.equal(assistant.assistantMetrics.firstTokenTime, 1040);
  assert.equal(assistant.input, 30);
  assert.equal(assistant.cacheRead, 10);
  assert.equal(assistant.output, 8);
  assert.equal(assistant.think, 3);
  assert.equal(tool.kind, 'tool');
  assert.equal(tool.text, 'file_read');
  assert.equal(tool.outputDetail, 'contents');
  assert.equal(tool.schemaDetail.includes('Read a file'), true);
  assert.equal(tool.timeSeconds, 0.05);

  assert.equal(projection.requestNumbers.length, 1);
  assert.equal(projection.requestNumbers[0].number, 1);
  assert.equal(projection.requestNumbers[0].provider, 'openai');
  assert.equal(projection.requestNumbers[0].usage.output, 8);
  assert.equal(projection.historyStartSeq, 1);
});

run('prompt changes become DeepSeek system update records', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'go' }),
    recorded(3, 1010, 'model_request', {
      step_index: 1,
      messages: [{ role: 'system', content: 'one' }],
      tools: [],
    }),
    recorded(4, 1020, 'model_response', { step_index: 1, content: 'first' }),
    recorded(5, 1030, 'model_request', {
      step_index: 2,
      messages: [{ role: 'system', content: 'two' }],
      tools: [],
    }),
    recorded(6, 1040, 'model_response', { step_index: 2, content: 'second' }),
  ]);

  const groups = projection.turns[0].groups;
  assert.deepEqual(groups.map((group) => group.title), ['Message', 'Step 1', 'Message', 'Step 2']);
  assert.equal(groups[2].cells[0].text, 'System Prompt Updated');
  assert.equal(groups[2].cells[0].previousPromptDetail.system, 'one');
  assert.equal(groups[2].cells[0].promptDetail.system, 'two');
});

run('legacy records keep empty timing without adding source or age notices', () => {
  const legacy = {
    schema_version: 1,
    sequence: null,
    legacy_index: 0,
    timestamp_ms: null,
    type: 'legacy_user_message',
    source: 'legacy',
    payload: { uuid: 'old-turn', content: 'old fact' },
  };
  const merged = mergeTrajectoryRecords(
    [recorded(2, 2000, 'done', {})],
    [legacy, recorded(1, 1900, 'model_response', { step_index: 1, content: 'answer' }), legacy],
  );
  assert.deepEqual(merged.map((record) => record.sequence), [null, 1, 2]);

  const projection = buildDeepSeekTrajectory(merged);
  assert.equal(projection.turns[0].groups[0].cells[0].previewMarkdown, 'old fact');
  assert.equal('source' in projection, false);
  assert.equal('missingCapabilities' in projection, false);
  assert.equal('diagnostics' in projection, false);
});

run('real legacy response keeps every null-sequence record and projects visible rows', () => {
  const responseRecords = [
    {
      schema_version: 1,
      sequence: null,
      legacy_index: 0,
      timestamp_ms: 1784969812398,
      type: 'legacy_user_message',
      source: 'legacy',
      payload: {
        uuid: '732dd38a-1d8e-4a3e-b416-6527f1ad55aa',
        content: 'convert this address',
      },
    },
    {
      schema_version: 1,
      sequence: null,
      legacy_index: 1,
      timestamp_ms: null,
      type: 'legacy_model_response',
      source: 'legacy',
      payload: {
        content: 'converted address',
        reasoning_content: 'checking address parts',
      },
    },
    {
      schema_version: 1,
      sequence: null,
      legacy_index: 2,
      timestamp_ms: 1784969817511,
      type: 'legacy_turn_end',
      source: 'legacy',
      payload: {
        turn_id: '732dd38a-1d8e-4a3e-b416-6527f1ad55aa',
        started_at_ms: 1784969812398,
        completed_at_ms: 1784969817511,
        duration_ms: 5113,
      },
    },
  ];

  const merged = mergeTrajectoryRecords([], responseRecords);
  assert.deepEqual(merged.map((record) => record.legacy_index), [0, 1, 2]);

  const projection = buildDeepSeekTrajectory(merged);
  assert.deepEqual(projection.nodes.map((node) => node.kind), ['user', 'assistant']);
  assert.equal(projection.turns.length, 1);
  assert.deepEqual(
    projection.turns[0].groups.flatMap((group) => group.cells).map((cell) => cell.kind),
    ['user', 'message'],
  );
  assert.equal(Number.isNaN(projection.nodes[1].time), true);
});

run('empty optional numeric fields remain missing instead of becoming zero', () => {
  const records = [null, undefined, '   '].map((timestamp, legacyIndex) => ({
    schema_version: 1,
    sequence: null,
    legacy_index: legacyIndex,
    timestamp_ms: timestamp,
    type: 'legacy_user_message',
    source: 'legacy',
    payload: {
      uuid: `empty-time-${legacyIndex}`,
      content: `message ${legacyIndex}`,
    },
  }));

  const merged = mergeTrajectoryRecords([], records);
  assert.equal(merged.length, 3);
  const projection = buildDeepSeekTrajectory(merged);
  assert.equal(projection.nodes.length, 3);
  assert.equal(projection.nodes.every((node) => Number.isNaN(node.time)), true);
});

run('turn_start aliases its user message into Turn 1 without creating a phantom turn', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', {
      turn_id: 'active-turn',
      user_message_id: 'user-message',
    }),
    recorded(2, 1001, 'message', {
      role: 'user', id: 'user-message', content: 'first turn',
    }),
    recorded(3, 1010, 'model_request', {
      step_index: 1, messages: [], tools: [],
    }),
  ]);
  assert.equal(projection.turns.length, 1);
  assert.equal(projection.turns[0].turn, 1);
  const user = projection.turns[0].groups
    .flatMap((group) => group.cells)
    .find((cell) => cell.kind === 'user');
  assert.equal(user.previewMarkdown, 'first turn');
});

run('live reasoning and text resolve to DeepSeek partial without rebuilding durable records', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'stream' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', { step_index: 1, messages: [], tools: [] }),
  ]);
  const partial = resolveDeepSeekTrajectoryPartial(projection, {
    turn: 1,
    step: 1,
    blocks: [
      { kind: 'reasoning', text: 'checking' },
      { kind: 'text', text: 'answering' },
    ],
  });
  assert.deepEqual(partial, {
    turn: 1,
    step: 1,
    blocks: [
      { kind: 'reasoning', text: 'checking' },
      { kind: 'text', text: 'answering' },
    ],
  });
});

run('running calls and retry metadata use the DeepSeek request contract', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'run' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', {
      step_index: 1,
      provider: 'openai',
      model: 'gpt-test',
      messages: [],
      tools: [{ name: 'bash', description: 'Run', parameters: { type: 'object' } }],
    }),
    recorded(5, 1020, 'agent_progress', {
      phase: 'model_retry', retry_attempt: 2, retry_max_attempts: 5, retry_delay_ms: 750,
    }),
    recorded(6, 1030, 'tool_start', {
      step_index: 1,
      tool: 'bash',
      tool_call_id: 'call-live',
      args: { command: 'pwd' },
      started_at_ms: 1030,
    }),
  ]);
  assert.equal(projection.runningCalls.length, 1);
  assert.equal(projection.runningCalls[0].callId, 'call-live');
  assert.equal(projection.callSchemas.get('call-live').description, 'Run');
  assert.equal(projection.requestNumbers[0].status, 'running');
  assert.equal(projection.requestNumbers[0].retry, 2);
  assert.equal(projection.requestNumbers[0].maxRetries, 5);
  assert.equal(projection.requestNumbers[0].retryDelayMs, 750);
});

run('failed requests without partial content stay request-only like DeepSeek', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'fail' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', { step_index: 1, messages: [], tools: [] }),
    recorded(5, 1020, 'model_response', {
      step_index: 1, status: 'error', content: '', error: { display_message: 'offline' },
    }),
    recorded(6, 1021, 'model_step_finish', { step_index: 1, reason: 'error' }),
  ]);
  const request = projection.requests[0];
  assert.equal(request.status, 'error');
  assert.equal('resultSeq' in request, false);
  assert.equal(projection.nodes.some((node) => node.kind === 'assistant'), false);
  const requestCell = projection.turns[0].groups
    .flatMap((group) => group.cells)
    .find((cell) => cell.requestOnly === true);
  assert.equal(requestCell.isError, true);
});

run('aborted partial content becomes DeepSeek interrupted assistant evidence', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'abort' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', { step_index: 1, messages: [], tools: [] }),
    recorded(5, 1020, 'model_response', {
      step_index: 1, status: 'aborted', content: 'partial answer',
    }),
    recorded(6, 1030, 'model_step_finish', { step_index: 1, reason: 'aborted' }),
  ]);
  const assistant = projection.nodes.find((node) => node.kind === 'assistant');
  assert.equal(assistant.interrupted, true);
  assert.equal(assistant.seq, 5.1);
  assert.equal(assistant.time, 1030);
  assert.equal(projection.requests[0].status, 'error');
  assert.equal('resultSeq' in projection.requests[0], false);
});

run('retry responses remain failed request runs instead of completed runs', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'retry' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', { step_index: 1, messages: [], tools: [] }),
    recorded(5, 1020, 'model_response', { step_index: 1, status: 'retry', content: '' }),
    recorded(6, 1030, 'model_step_finish', { step_index: 1, reason: 'retry' }),
  ]);
  assert.equal(projection.requests[0].status, 'error');
  assert.equal(projection.requestNumbers[0].status, 'error');
});

run('compact notices project as the canonical standalone Compaction request', () => {
  const compact = (sequence, stage, content, complete = false) => recorded(
    sequence,
    1000 + sequence,
    'message',
    {
      role: 'system',
      content,
      metadata: {
        compact_notice: true,
        compact_notice_id: 'compact-1',
        compact_notice_stage: stage,
        compact_notice_complete: complete,
      },
    },
  );
  const projection = buildDeepSeekTrajectory([
    compact(1, 'progress', 'Compacting context…'),
    compact(2, 'summary', '[Conversation summary] condensed', true),
  ]);
  const request = projection.requests[0];
  assert.equal(request.purpose, 'compaction');
  assert.equal(request.status, 'complete');
  assert.equal(request.summary[0].text, 'condensed');
  assert.equal(projection.turns[0].turn, null);
  assert.equal(projection.turns[0].groups[0].cells[0].kind, 'compacted');
});

// 场景：文本工具调用被拒后本步以 text_tool_call_retry 结束（fix-feedback-0924 第 3 条）。
// 期望：与空回复重试一样归为失败的请求，而不是「已完成」。回归表现：纠正重试的那一步在
// trajectory 里显示为成功，看不出模型那次回复其实没被执行。
run('text-form tool call retry steps are shown as failed requests', () => {
  const projection = buildDeepSeekTrajectory([
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1' }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'ls' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', { step_index: 1, messages: [], tools: [] }),
    recorded(5, 1020, 'model_response', { step_index: 1, status: 'complete', content: '' }),
    recorded(6, 1030, 'model_step_finish', { step_index: 1, reason: 'text_tool_call_retry' }),
  ]);
  assert.equal(projection.requests[0].status, 'error');
});
