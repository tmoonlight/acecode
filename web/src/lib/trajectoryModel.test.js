import assert from 'node:assert/strict';

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
