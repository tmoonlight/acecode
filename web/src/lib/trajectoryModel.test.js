import assert from 'node:assert/strict';

import {
  buildDeepSeekTrajectory,
  mergeTrajectoryRecords,
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
