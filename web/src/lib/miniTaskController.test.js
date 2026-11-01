import assert from 'node:assert/strict';
import { createMiniTaskController, MINI_FILE_LIMIT } from './miniTaskController.js';

const flush = async () => { for (let i = 0; i < 8; i++) await new Promise(resolve => setImmediate(resolve)); };
const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
function fixture(extra = {}) {
  let id = 0, context = { workspaceHash: 'project-a' };
  const calls = { create: [], send: [], upload: [], capture: 0, canceled: 0, published: [] };
  const recorder = { stop: () => new Blob(['wav']), cancel: () => calls.canceled++ };
  const client = {
    createWorkspaceSession: async (workspace, options) => { calls.create.push({ workspace, options }); return { id: `session-${calls.create.length}`, workspace_hash: workspace }; },
    createSession: async options => { calls.create.push({ options }); return { id: `session-${calls.create.length}` }; },
    sendInput: async (sessionId, payload) => { calls.send.push({ sessionId, payload }); return { queued: true }; },
    uploadSessionAttachment: async (sessionId, payload) => { calls.upload.push({ sessionId, payload }); return { id: 'attachment-1' }; },
    transcribeSpeech: async () => ({ text: '录音任务', status: 'ok' }), cancelSpeech: async () => {},
    ...extra.client,
  };
  const controller = createMiniTaskController({ client, getContext: () => context, createId: () => `request-${++id}`,
    publish: value => calls.published.push(value),
    getSpeechConfig: async () => ({ enabled: true, configured: true, max_recording_seconds: 60, timeout_ms: 1000 }),
    capture: async options => { calls.capture++; calls.captureOptions = options; return recorder; },
    ...extra.options,
  });
  return { controller, calls, client, recorder, setContext: next => { context = next; } };
}

{
  const pending = deferred();
  const f = fixture({ client: { sendInput: async (...args) => { f.calls.send.push(args); return pending.promise; } } });
  const action = { type: 'mini-submit', request_id: 'text-1', text: '  实现功能  ', files: [] };
  const submitted = f.controller.handle(action); await flush();
  await f.controller.handle(action);
  assert.equal(f.calls.create.length, 1); assert.equal(f.calls.send.length, 1);
  assert.equal(f.calls.create[0].workspace, 'project-a');
  assert.equal(f.calls.create[0].options.auto_start, false);
  assert.equal(f.controller.getSnapshot().pendingTasks.length, 0, 'count waits for acceptance');
  pending.resolve({ queued: true }); await submitted;
  assert.equal(f.controller.getSnapshot().pendingTasks.length, 1);
  await f.controller.handle(action); assert.equal(f.calls.create.length, 1, 'replayed acknowledgement cannot create another task');
  f.controller.observeActivities([{ sessionId: 'session-1' }]);
  assert.equal(f.controller.getSnapshot().pendingTasks.length, 0, 'real activity replaces pending count');
  f.controller.dispose();
}
{
  const f = fixture(); let fail = true;
  f.client.sendInput = async (id, payload) => { f.calls.send.push({ id, payload }); if (fail) throw new Error('offline'); return { queued: true }; };
  for (const action of [
    { type: 'mini-file-begin', id: 'file-1', name: 'notes.txt', mime: 'text/plain', size: 3 },
    { type: 'mini-file-chunk', id: 'file-1', offset: 0, data: 'YWJj' },
    { type: 'mini-file-end', id: 'file-1' },
  ]) f.controller.handle(action);
  const action = { type: 'mini-submit', request_id: 'retry-1', text: 'read', files: ['file-1'] };
  await f.controller.handle(action);
  assert.equal(f.controller.getSnapshot().error, 'offline');
  assert.equal(f.controller.getSnapshot().pendingTasks.length, 0);
  assert.equal(f.controller.getSnapshot().files.length, 1, 'failure retains files');
  fail = false; await f.controller.handle(action);
  assert.equal(f.calls.create.length, 1); assert.equal(f.calls.upload.length, 1);
  assert.deepEqual(f.calls.send[1].payload.attachments, [{ id: 'attachment-1' }]);
  assert.equal(f.controller.getSnapshot().files.length, 0);
  f.controller.dispose();
}
{
  const f = fixture();
  f.controller.handle({ type: 'mini-file-begin', id: 'oversize', size: MINI_FILE_LIMIT + 1 });
  assert.equal(f.controller.getSnapshot().files.length, 0);
  f.controller.handle({ type: 'mini-file-begin', id: 'bad', size: 3, name: 'bad.txt' });
  f.controller.handle({ type: 'mini-file-chunk', id: 'bad', offset: 8, data: 'YWJj' });
  assert.ok(f.controller.getSnapshot().files[0].error);
  await f.controller.handle({ type: 'mini-submit', request_id: 'bad-input', text: '', files: ['bad'] });
  assert.equal(f.calls.create.length, 0);
  f.controller.dispose();
}
{
  const permission = deferred(); const f = fixture({ options: { capture: () => permission.promise } });
  const started = f.controller.handle({ type: 'mini-record-start' }); await flush();
  f.setContext({ workspaceHash: 'project-b' });
  f.controller.handle({ type: 'mini-record-stop' });
  permission.resolve(f.recorder); await started; await flush();
  assert.equal(f.calls.create[0].workspace, 'project-a', 'recording preserves workspace captured on click');
  assert.equal(f.calls.send[0].payload.text, '录音任务');
  assert.equal(f.controller.getSnapshot().pendingTasks.length, 1);
  f.controller.dispose();
}
{
  const f = fixture();
  await f.controller.handle({ type: 'mini-record-start' });
  f.calls.captureOptions.onLimit(); await flush();
  assert.equal(f.calls.send.length, 1, 'duration limit also sends without main composer');
  f.controller.dispose();
}
{
  const f = fixture({ client: { sendInput: async () => { throw new Error('send failed'); } } });
  await f.controller.handle({ type: 'mini-record-start' });
  await f.controller.handle({ type: 'mini-record-stop' }); await flush();
  const recovered = f.controller.getSnapshot().recovery;
  assert.equal(recovered.text, '录音任务');
  f.client.sendInput = async (...args) => { f.calls.send.push(args); return { queued: true }; };
  await f.controller.handle({ type: 'mini-submit', request_id: recovered.requestId, text: recovered.text });
  assert.equal(f.calls.create.length, 1);
  f.controller.dispose();
}
for (const dispose of [false, true]) {
  const permission = deferred(); const f = fixture({ options: { capture: () => permission.promise } });
  const started = f.controller.handle({ type: 'mini-record-start' }); await flush();
  if (dispose) f.controller.dispose(); else f.controller.handle({ type: 'mini-record-cancel' });
  permission.resolve(f.recorder); await started; await flush();
  assert.equal(f.calls.create.length, 0); assert.equal(f.calls.canceled, 1, 'late microphone stream is released');
  f.controller.dispose();
}
{
  const config = deferred(); const f = fixture({ options: { getSpeechConfig: () => config.promise } });
  const started = f.controller.handle({ type: 'mini-record-start' });
  f.controller.handle({ type: 'mini-record-cancel' });
  config.resolve({ enabled: true, configured: true }); await started;
  assert.equal(f.calls.capture, 0, 'cancel during config load never requests microphone');
  f.controller.dispose();
}
for (const config of [false, true]) {
  const f = fixture(config ? { client: { transcribeSpeech: async () => ({ status: 'no_speech', text: '' }) } }
    : { options: { getSpeechConfig: async () => ({ enabled: false, configured: false }) } });
  await f.controller.handle({ type: 'mini-record-start' });
  await f.controller.handle({ type: 'mini-record-stop' }); await flush();
  assert.equal(f.calls.create.length, 0); assert.ok(f.controller.getSnapshot().error);
  f.controller.dispose();
}
{
  const create = deferred(); const f = fixture({ client: { createWorkspaceSession: () => create.promise } });
  const submitted = f.controller.handle({ type: 'mini-submit', request_id: 'closing', text: 'work' });
  f.controller.dispose(); create.resolve({ id: 'late' }); await submitted;
  assert.equal(f.calls.send.length, 0, 'closing mini before creation finishes cannot start a late task');
}
console.log('[pass] mini background task acceptance, retry, attachments, independent recording, stop-before-permission, cancellation and teardown');
