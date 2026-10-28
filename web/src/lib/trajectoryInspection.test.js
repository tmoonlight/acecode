import assert from 'node:assert/strict';
import { buildDeepSeekTrajectory, resolveDeepSeekTrajectoryPartial } from './trajectoryModel.js';
import { appendTrajectoryPartialLayout } from '../components/trajectory/deepseek/layout.ts';
import { codeProgram } from '../components/trajectory/deepseek/code-program.ts';
import { trajectorySourceBlock, safeTrajectoryImageSource } from '../components/trajectory/deepseek/trajectory-attachments.ts';
import { trajectoryStringWrapping } from '../components/trajectory/deepseek/string-wrapping-store.ts';
import { trajectoryHistoryWindow } from './trajectoryHistoryWindow.js';

function run(name, fn) {
  fn();
  console.log(`[pass] ${name}`);
}
const event = (sequence, type, payload = {}, timestamp_ms = sequence * 100) => ({ sequence, type, payload, timestamp_ms });
const prefix = [
  event(1, 'message', { role: 'user', id: 'u1', content: 'inspect' }),
  event(2, 'turn_start', { turn_id: 'u1', user_message_id: 'u1' }),
  event(3, 'model_step_start', { step_index: 1 }),
  event(4, 'model_request', { step_index: 1, messages: [], tools: [] }),
];
const cells = projection => projection.turns.flatMap(turn => turn.groups.flatMap(group => group.cells));

run('retrying stays pending and recovery retains the last attempt error and first-token time', () => {
  const history = [...prefix,
    event(5, 'model_first_output', { step_index: 1 }),
    event(6, 'model_response', { step_index: 1, status: 'retry', error: { display_message: 'temporary offline' } }),
    event(7, 'agent_progress', { phase: 'model_retry', retry_attempt: 1, retry_max_attempts: 5, retry_delay_ms: 750 }),
  ];
  const pending = buildDeepSeekTrajectory(history);
  assert.equal(pending.requests[0].status, 'running');
  assert.equal(pending.requests[0].completedAt, null);
  assert.equal(pending.nodes.some(node => node.kind === 'assistant'), false);
  const partial = resolveDeepSeekTrajectoryPartial(pending, { turnId: 'u1', step: 1, blocks: [{ kind: 'text', text: 'live' }] });
  const liveCell = appendTrajectoryPartialLayout([], partial, 0)[0].groups[0].cells[0];
  assert.equal(liveCell.startedAt, 300);
  assert.equal(liveCell.timeSeconds, null);
  assert.equal(liveCell.assistantMetrics.firstTokenTime, 500);
  assert.equal(liveCell.assistantMetrics.completedTime, null);
  const recovered = buildDeepSeekTrajectory([...history,
    event(8, 'model_first_output', { step_index: 1 }),
    event(9, 'model_response', { step_index: 1, status: 'completed', content: 'recovered', usage: { output_tokens: 8 } }),
  ]);
  assert.equal(recovered.requests[0].status, 'complete');
  assert.equal(recovered.requestNumbers[0].error, 'temporary offline');
  assert.equal(recovered.requestNumbers[0].retry, 1);
  assert.equal(cells(recovered).find(cell => cell.kind === 'message').assistantMetrics.firstTokenTime, 500);
});

run('settled response timing and status are independent of later tool execution or cancellation', () => {
  const history = [...prefix,
    event(5, 'model_first_output', { step_index: 1 }),
    event(6, 'model_response', { step_index: 1, status: 'completed', content: 'running tool', tool_calls: [{ id: 'tool-1', name: 'bash', arguments: '{}' }] }),
    event(7, 'tool_start', { step_index: 1, tool: 'bash', tool_call_id: 'tool-1', args: {} }),
    event(8, 'model_step_finish', { step_index: 1, reason: 'aborted' }, 6000),
  ];
  const projection = buildDeepSeekTrajectory(history);
  assert.equal(projection.requests[0].status, 'complete');
  assert.equal(projection.requests[0].completedAt, 600);
  const assistant = cells(projection).find(cell => cell.kind === 'message');
  assert.equal(assistant.timeSeconds, 0.3);
  assert.equal(assistant.assistantMetrics.completedTime, 600);
  assert.notEqual(assistant.isError, true);
});

run('turn ending without a model response leaves an inspectable failed request with no result link', () => {
  const projection = buildDeepSeekTrajectory([...prefix, event(5, 'turn_end', { turn_id: 'u1', reason: 'aborted' })]);
  assert.equal(projection.requests[0].status, 'error');
  assert.equal(projection.requests[0].completedAt, 500);
  assert.equal(projection.requests[0].resultSeq, undefined);
});

run('attachments preserve order, duplicate references, zero byte files, original metadata and image safety', () => {
  const image = { type: 'image', attachment: { id: 'img', name: 'C:\\pics\\photo.png', mime_type: 'image/png', size: 10, blob_url: '/api/sessions/s/attachments/img/blob' } };
  const file = { type: 'file', attachment: { id: 'file', name: 'empty.txt', size: 0, source_path: 'C:\\private\\empty.txt' } };
  const projection = buildDeepSeekTrajectory([event(1, 'message', {
    role: 'user', id: 'u1', content_parts: [{ type: 'text', text: 'mixed' }, image, file, image],
  })]);
  const user = cells(projection).find(cell => cell.kind === 'user');
  assert.equal(user.text, 'Images ×2 · Files ×1');
  assert.equal(user.sourceBlocks.length, 4);
  assert.equal(user.sourceBlocks[1].attachment.name, 'photo.png');
  assert.equal(user.sourceBlocks[2].file.bytes, 0);
  assert.deepEqual(JSON.parse(user.sourceBlocks[2].content), file);
  assert.deepEqual(user.sourceBlocks[1], user.sourceBlocks[3]);
  for (const unsafe of ['javascript:alert(1)', 'file:///C:/secret.png', '//evil.invalid/a', '/api/config', 'data:text/html;base64,QQ==']) {
    assert.equal(safeTrajectoryImageSource(unsafe), undefined);
  }
  assert.equal(trajectorySourceBlock({ type: 'image_url', image_url: { url: 'https://example.org/a.png' } }).attachment.src, 'https://example.org/a.png');
});

run('recorded run_code identity and schema control inspection without rewriting source or JSON', () => {
  const raw = '{ "code": "print(1)\\n", "description": "test" }';
  const cell = { kind: 'tool', toolName: 'run_code', inputDetail: raw, schemaDetail: JSON.stringify({ parameters: { properties: { code: { description: 'Python program' } } } }) };
  assert.equal(codeProgram(cell).source, 'print(1)\n');
  assert.equal(codeProgram(cell).rawInput, raw);
  assert.equal(codeProgram(cell).language, 'python');
  assert.equal(codeProgram({ ...cell, toolName: 'bash', text: 'run_code' }), undefined);
  assert.equal(codeProgram({ ...cell, schemaDetail: cell.schemaDetail.replace('Python', 'Python or TypeScript') }).language, undefined);
  assert.equal(codeProgram({ ...cell, inputDetail: '{' }), undefined);
  assert.equal(codeProgram({ ...cell, inputDetail: '{"code":1}' }), undefined);
});

run('wrapping preference survives unavailable or rejecting browser storage', () => {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, 'localStorage');
  try {
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, get() { throw new Error('denied'); } });
    trajectoryStringWrapping.setDefault(true);
    assert.equal(trajectoryStringWrapping.getDefault(), true);
    trajectoryStringWrapping.setDefault(false);
    assert.equal(trajectoryStringWrapping.getDefault(), false);
  } finally {
    if (descriptor) Object.defineProperty(globalThis, 'localStorage', descriptor);
    else delete globalThis.localStorage;
  }
});

run('resident history window anchors streaming appends, reveals older rows and reanchors replacement', () => {
  const nodes = Array.from({ length: 120 }, (_, index) => ({ seq: index + 1 }));
  const requests = [{ startSeq: 1, resultSeq: 10 }, { startSeq: 65, resultSeq: 72 }, { startSeq: 100 }];
  const initial = trajectoryHistoryWindow(nodes, requests);
  assert.equal(initial.nodes[0].seq, 71);
  assert.equal(initial.tailSeq, 120);
  assert.equal(initial.requests.length, 2);
  const appended = trajectoryHistoryWindow([...nodes, { seq: 121 }], requests, initial.tailSeq);
  assert.equal(appended.nodes[0].seq, 71);
  assert.equal(appended.nodes.length, 51);
  assert.equal(trajectoryHistoryWindow(nodes, requests, initial.tailSeq, 100).nodes[0].seq, 21);
  const replaced = trajectoryHistoryWindow([{ seq: 1000 }, { seq: 1001 }], [], initial.tailSeq);
  assert.equal(replaced.tailSeq, 1001);
  assert.equal(replaced.startIndex, 0);
});
