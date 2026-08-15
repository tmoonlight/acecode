import assert from 'node:assert/strict';

import {
  buildTrajectoryViewModel,
  formatTrajectoryDuration,
  mergeTrajectoryRecords,
  trajectoryMatches,
  trajectoryTimelineSegments,
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

run('trajectory projection groups precise model and tool lifecycles by turn', () => {
  const records = [
    recorded(1, 1000, 'turn_start', { turn_id: 'turn-1', started_at_ms: 1000 }),
    recorded(2, 1001, 'message', { role: 'user', id: 'turn-1', content: 'inspect repo' }),
    recorded(3, 1010, 'model_step_start', { step_index: 1 }),
    recorded(4, 1011, 'model_request', {
      step_index: 1,
      provider: 'openai',
      model: 'gpt-test',
      messages: [{ role: 'user', content: 'inspect repo' }],
      tools: [{ name: 'read', native_name: 'file_read', parameters: { type: 'object' } }],
    }),
    recorded(5, 1040, 'model_first_output', { step_index: 1, channel: 'reasoning' }),
    recorded(6, 1100, 'model_response', {
      step_index: 1,
      status: 'completed',
      reasoning_content: 'need a file',
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
    recorded(10, 1200, 'turn_end', {
      turn_id: 'turn-1',
      completed_at_ms: 1200,
      duration_ms: 200,
      outcome: 'completed',
    }),
  ];

  const model = buildTrajectoryViewModel(records);
  assert.equal(model.turns.length, 1);
  assert.equal(model.turns[0].outcome, 'completed');
  assert.equal(model.turns[0].rows.length, 3);
  const modelRow = model.turns[0].rows[1];
  const toolRow = model.turns[0].rows[2];
  assert.equal(modelRow.category, 'model');
  assert.equal(modelRow.ttftMs, 30);
  assert.equal(modelRow.details.payload.messages[0].content, 'inspect repo');
  assert.equal(toolRow.details.payload.file_path, 'a');
  assert.equal(toolRow.details.result.output, 'contents');
  assert.equal(toolRow.details.schema.name, 'read');
  assert.equal(toolRow.details.schema.native_name, 'file_read');
  assert.equal(toolRow.durationMs, 50);
  assert.equal(trajectoryTimelineSegments(model, 'calls').length, 2);
  assert.deepEqual(trajectoryMatches(model, 'contents'), [toolRow.key]);
});

run('legacy records stay before precise suffix and preserve unknown timing', () => {
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
    [legacy, recorded(1, 1900, 'model_step_start', { step_index: 1 }), legacy],
  );
  assert.deepEqual(merged.map((record) => record.sequence), [null, 1, 2]);

  const model = buildTrajectoryViewModel(merged, ['ttft']);
  assert.equal(model.turns[0].source, 'legacy');
  assert.equal(model.turns[0].rows[0].startMs, null);
  assert.equal(model.missingCapabilities[0], 'ttft');
  assert.equal(formatTrajectoryDuration(null), '未记录');
});

run('point-in-time session events keep timestamps and concise labels', () => {
  const model = buildTrajectoryViewModel([
    recorded(1, 1000, 'busy_changed', { busy: true }),
    recorded(2, 1010, 'message', { role: 'error', content: 'provider unavailable' }),
    recorded(3, 1020, 'done', {}),
  ]);

  assert.equal(model.rows[0].preview, '运行中');
  assert.equal(model.rows[0].durationMs, null);
  assert.equal(model.rows[1].label, '错误消息');
  assert.equal(model.rows[1].preview, 'provider unavailable');
  assert.equal(model.rows[2].preview, '完成');
  assert.equal(trajectoryTimelineSegments(model)[0].durationMs, null);
});
