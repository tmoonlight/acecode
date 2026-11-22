import assert from 'node:assert/strict';
import fs from 'node:fs';
import { annotationIsSendable, createAnnotationBatchSender, formatAnnotationBatch, localizedSendError, pageAnnotationBatchSender } from './pageAnnotationBatches.js';

const record = (number, fields = {}) => ({ id: `record-${number}`, number, status: 'pending', valid: true,
  document_id: 'runtime-doc', description: `Request ${number}`, targets: [{ id: `target-${number}` }],
  differences: [], region: null, ...fields });
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };

function fixture(options = {}) {
  const calls = [];
  const snapshot = { document_id: 'runtime-doc', records: [record(1), record(2, { description: '', differences: [{ target_id: 'target-2', property: 'color', old_value: 'red', new_value: 'blue' }] }), record(3, { description: '  ' })] };
  const invoke = async (page, command) => {
    calls.push({ action: command.action, page, command: structuredClone(command) });
    if (options.native) {
      const result = await options.native(command);
      if (result) return result;
    }
    if (command.action === 'capture') return { ok: true, page_id: page,
      annotation_document_id: 'runtime-doc', result: { fullPage: true, documentId: 'different-native-capture-doc', data: 'image' } };
    if (command.action === 'acknowledge') {
      for (const item of snapshot.records) if (command.record_ids.includes(item.id)) item.status = 'sent';
    }
    return { ok: true, page_id: page, annotations: structuredClone(snapshot) };
  };
  const transport = {
    async uploadSessionAttachment(session, body) {
      calls.push({ action: 'upload', session, body });
      return options.upload ? options.upload() : { attachment: { id: 'image-1' } };
    },
    async sendInput(session, payload, mode) {
      calls.push({ action: 'send', session, payload: structuredClone(payload), ...(mode ? { mode } : {}) });
      return options.send ? options.send(payload, mode) : { queued: true, annotation_batch_id: payload.annotation_batch_id };
    },
  };
  const api = { snapshotConnection: () => transport };
  let nextBatch = 0;
  const sender = createAnnotationBatchSender({ pageId: 'page-1', invoke, makeId: () => `batch-${++nextBatch}` });
  const context = { api, sessionId: 'session-1', snapshot, page: { url: 'https://example.com', title: 'Example' } };
  return { calls, sender, context, snapshot, transport };
}

{
  const test = fixture();
  const before = structuredClone(test.snapshot.records);
  assert.equal((await test.sender.send(test.context)).ok, true);
  assert.deepEqual(test.calls.map(call => call.action), ['capture', 'upload', 'validate', 'send', 'release', 'acknowledge']);
  const sent = test.calls.find(call => call.action === 'send');
  assert.equal(sent.session, 'session-1');
  assert.deepEqual(Object.keys(sent.payload).sort(), ['annotation_batch_id', 'attachments', 'text']);
  assert.deepEqual(sent.payload.attachments, [{ id: 'image-1' }]);
  assert.match(sent.payload.text, /Request 1/);
  assert.match(sent.payload.text, /"new_value": "blue"/);
  assert.doesNotMatch(sent.payload.text, /"number": 3/);
  assert.deepEqual(test.calls[0].command.record_ids, ['record-1', 'record-2']);
  assert.equal(test.snapshot.records[2].status, 'pending');
  assert.deepEqual(test.snapshot.records.map(item => ({ ...item, status: 'pending' })), before);
  assert.equal(annotationIsSendable(record(1, { status: 'sent' })), false);
  assert.match(formatAnnotationBatch([record(1)], {}), /Page annotations/);
}

{
  const gate = deferred();
  const test = fixture({ native: async command => { if (command.action === 'capture') await gate.promise; } });
  const first = test.sender.send(test.context);
  const concurrent = test.sender.send(test.context);
  assert.equal(first, concurrent);
  test.snapshot.records[0].description = 'Changed during capture';
  test.snapshot.records.push(record(4));
  test.context.sessionId = 'session-2';
  gate.resolve();
  assert.equal((await first).ok, true);
  const sent = test.calls.find(call => call.action === 'send');
  assert.equal(sent.session, 'session-1');
  assert.doesNotMatch(sent.payload.text, /Changed during capture|"number": 4/);
  assert.equal(test.snapshot.records[3].status, 'pending');
  assert.equal((await test.sender.send(test.context)).ok, true);
  const next = test.calls.filter(call => call.action === 'send')[1];
  assert.equal(next.session, 'session-2');
  assert.equal(next.payload.annotation_batch_id, 'batch-2');
  assert.match(next.payload.text, /"number": 4/);
  assert.doesNotMatch(next.payload.text, /"number": [123]/);
}

for (const receipt of [null, {}, { queued: true }, { queued: true, annotation_batch_id: 'another' },
  { queued: false, annotation_batch_id: 'batch-1' }, 'network']) {
  let attempt = 0;
  const test = fixture({ send: payload => {
    if (++attempt === 1) { if (receipt === 'network') throw new Error('connection lost'); return receipt; }
    return { queued: true, annotation_batch_id: payload.annotation_batch_id, duplicate: true };
  } });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.some(call => call.action === 'acknowledge'), false);
  assert.equal(test.snapshot.records[0].status, 'pending');
  const frozen = test.calls.find(call => call.action === 'send');
  const otherApi = { snapshotConnection() { throw new Error('must keep original transport'); } };
  assert.equal((await test.sender.send({ ...test.context, api: otherApi })).ok, true);
  const sends = test.calls.filter(call => call.action === 'send');
  assert.deepEqual(sends[1].payload, frozen.payload);
  assert.equal(sends[1].session, frozen.session);
  assert.deepEqual(sends[1].mode, { reconcileOnly: true });
  assert.equal(test.calls.filter(call => call.action === 'capture').length, 1);
  assert.equal(test.calls.filter(call => call.action === 'upload').length, 1);
}

for (const stage of ['capture', 'upload', 'validate', 'acknowledge']) {
  let failed = false;
  const test = fixture({
    native(command) {
      if (command.action === stage && !failed) { failed = true; throw new Error(`${stage} failed`); }
    },
    upload() {
      if (stage === 'upload' && !failed) { failed = true; throw new Error('upload failed'); }
      return { attachment: { id: 'image-1' } };
    },
  });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.snapshot.records[0].status, 'pending');
  assert.equal(test.calls.filter(call => call.action === 'send').length, stage === 'acknowledge' ? 1 : 0);
  assert.equal((await test.sender.send(test.context)).ok, true);
  assert.equal(test.calls.filter(call => call.action === 'send').length, 1);
  assert.equal(test.calls.filter(call => call.action === 'capture').length, stage === 'capture' ? 2 : 1);
}

for (const badCapture of [
  { annotation_document_id: 'capture-token-must-not-be-runtime-token', result: { fullPage: true, documentId: 'capture-doc', data: 'bytes' } },
  { annotation_document_id: 'runtime-doc', result: { fullPage: false, documentId: 'capture-doc', data: 'bytes' } },
  { annotation_document_id: 'runtime-doc', result: { fullPage: true, documentId: '', data: 'bytes' } },
]) {
  const test = fixture({ native: command => command.action === 'capture' ? { ok: true, page_id: 'page-1', ...badCapture } : undefined });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.some(call => call.action === 'upload' || call.action === 'send'), false);
}

{
  assert.equal(localizedSendError('annotation_target_or_document_changed'), '批注目标已失效，请重新选择元素后再发送');
  assert.equal(localizedSendError('Annotation document or target is invalid'), '批注目标已失效，请重新选择元素后再发送');
  assert.equal(localizedSendError('capture_document_changed'), '网页内容已变化，请重新批注后再发送');
  assert.equal(localizedSendError('Annotation page is unavailable'), '批注页面不可用');
  assert.equal(localizedSendError('Annotation capture is already running'), '截图正在进行中，请稍后再试');
  assert.equal(localizedSendError('network down'), 'network down');
  const test = fixture({ native: command => command.action === 'capture'
    ? { ok: false, page_id: 'page-1', error: 'annotation_target_or_document_changed' } : undefined });
  const result = await test.sender.send(test.context);
  assert.equal(result.ok, false);
  assert.equal(result.error, '批注目标已失效，请重新选择元素后再发送');
  assert.equal(test.sender.getSnapshot().error, '批注目标已失效，请重新选择元素后再发送');
  console.log('[pass] native annotation send error codes are localized before reaching the toolbar');
}

for (const stage of ['capture', 'validate', 'acknowledge']) {
  const test = fixture({ native: command => command.action === stage
    ? { ok: false, page_id: 'page-1', error: 'target removed or document replaced' } : undefined });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.snapshot.records[0].status, 'pending');
  assert.equal(test.calls.filter(call => call.action === 'send').length, stage === 'acknowledge' ? 1 : 0);
}

for (const mutate of [
  test => { test.context.sessionId = ''; },
  test => { test.snapshot.document_id = ''; },
  test => { test.snapshot.records[0].valid = false; },
  test => { test.snapshot.records[0].document_id = 'old-document'; },
  test => { test.snapshot.records = [record(3, { description: '' })]; },
]) {
  const test = fixture(); mutate(test);
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.deepEqual(test.calls, []);
}

{
  const test = fixture({ send() { throw new Error('unknown receipt'); } });
  await test.sender.send(test.context);
  const count = test.calls.length;
  assert.equal((await test.sender.send({ ...test.context, sessionId: 'session-2' })).ok, false);
  assert.equal(test.calls.length, count);
}

{
  // Two different preconditions used to share one copy: annotations need a
  // conversation to land in, and the transport needs to be usable.
  const test = fixture();
  const noSession = await test.sender.send({ ...test.context, sessionId: '' });
  assert.equal(noSession.ok, false);
  assert.equal(noSession.error, '请先选择对话');
  assert.deepEqual(test.calls, []);
  const noApi = await test.sender.send({ ...test.context, api: null });
  assert.equal(noApi.ok, false);
  assert.equal(noApi.error, '批注页面不可用');
  assert.deepEqual(test.calls, []);
}

{
  const test = fixture({ upload: () => ({ attachment: {} }) });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.some(call => call.action === 'send'), false);
}

{
  const api = {};
  const first = pageAnnotationBatchSender('retention-page', api);
  assert.equal(pageAnnotationBatchSender('retention-page', api), first);
  const unsubscribe = first.subscribe(() => {});
  unsubscribe();
  first.subscribe(() => {});
  assert.equal(pageAnnotationBatchSender('retention-page', api), first);
  assert.notEqual(pageAnnotationBatchSender('another-page', api), first);
  assert.notEqual(pageAnnotationBatchSender('retention-page', {}), first);
}

{
  const source = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
  const sender = fs.readFileSync(new URL('./pageAnnotationBatches.js', import.meta.url), 'utf8');
  const component = fs.readFileSync(new URL('../components/PageAnnotationSendBar.jsx', import.meta.url), 'utf8');
  assert.match(source, /createAgentBrowserElementContext/);
  assert.match(source, /toggleAgentBrowserElementSelection/);
  assert.doesNotMatch(sender + component, /steerTurn|interruptTurn|setComposer|composerAttachments|onAddContext/);
  assert.match(source, /<PageAnnotationSendBar/);
  // The toolbar explains the missing conversation instead of letting the click
  // fail with an error message.
  assert.match(component, /disabled=\{!sender \|\| !sessionId \|\|/);
  assert.match(component, /\{!sessionId && <span className="ace-annotation-toolbar-note">请先选择对话<\/span>\}/);
}

{
  let valid = true;
  let admissions = 0;
  const test = fixture({
    native: command => command.action === 'validate' && !valid
      ? { ok: false, page_id: 'page-1', error: 'target removed' } : undefined,
    send(payload, mode) {
      if (mode?.reconcileOnly) throw Object.assign(new Error('not accepted'), { code: 'annotation_batch_not_accepted', status: 404 });
      if (++admissions === 1) throw new Error('request lost before receiver');
      return { queued: true, annotation_batch_id: payload.annotation_batch_id };
    },
  });
  assert.equal((await test.sender.send(test.context)).ok, false);
  valid = false;
  test.snapshot.records[0].valid = false;
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(admissions, 1, 'an invalid uncertain batch may reconcile but must never newly admit');
  assert.equal(test.snapshot.records[0].status, 'pending');
}

for (const code of ['annotation_batch_unavailable', 'annotation_batch_capacity', 'annotation_batch_conflict']) {
  let valid = true;
  const test = fixture({
    native: command => command.action === 'validate' && !valid
      ? { ok: false, error: 'invalid document' } : undefined,
    send() { throw Object.assign(new Error('rejected'), { code, status: code.endsWith('conflict') ? 409 : 503 }); },
  });
  assert.equal((await test.sender.send(test.context)).ok, false);
  valid = false;
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.filter(call => call.action === 'send').length, 1, 'explicit refusal must revalidate before another admission');
  assert.equal(test.calls.filter(call => call.action === 'validate').length, 2);
}

{
  let valid = true;
  let acceptedPayload;
  const test = fixture({
    native: command => command.action === 'validate' && !valid
      ? { ok: false, error: 'invalid document' } : undefined,
    send(payload, mode) {
      if (!mode?.reconcileOnly) { acceptedPayload = structuredClone(payload); throw new Error('receipt lost after admission'); }
      assert.deepEqual(payload, acceptedPayload);
      return { queued: true, annotation_batch_id: payload.annotation_batch_id, duplicate: true };
    },
  });
  assert.equal((await test.sender.send(test.context)).ok, false);
  valid = false;
  test.snapshot.records[0].valid = false;
  assert.equal((await test.sender.send(test.context)).ok, true, 'real existing receipt can acknowledge an invalidated document');
  assert.equal(test.calls.filter(call => call.action === 'validate').length, 1);
  assert.equal(test.snapshot.records[0].status, 'sent');
}

{
  let admissions = 0;
  const test = fixture({ send(payload, mode) {
    if (mode?.reconcileOnly) throw Object.assign(new Error('missing'), { code:'annotation_batch_not_accepted', status:404 });
    if (++admissions === 1) throw new Error('request never arrived');
    return {queued:true, annotation_batch_id:payload.annotation_batch_id};
  } });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal((await test.sender.send(test.context)).ok, true);
  assert.deepEqual(test.calls.map(call => call.action), ['capture', 'upload', 'validate', 'send', 'release', 'send', 'validate', 'send', 'release', 'acknowledge']);
  const sends = test.calls.filter(call => call.action === 'send');
  assert.deepEqual(sends[0].payload, sends[1].payload);
  assert.deepEqual(sends[0].payload, sends[2].payload);
  assert.equal(sends[2].mode, undefined);
}

for (const lookupResult of ['malformed', 'wrong-id', 'generic-404', 'missing-wrong-status', 'network']) {
  const test = fixture({ send(payload, mode) {
    if (!mode?.reconcileOnly) throw new Error('uncertain');
    if (lookupResult === 'malformed') return {};
    if (lookupResult === 'wrong-id') return { queued:true, annotation_batch_id:'another' };
    if (lookupResult === 'generic-404') throw Object.assign(new Error('unknown session'), { status:404 });
    if (lookupResult === 'missing-wrong-status') throw Object.assign(new Error('missing'), { status:503, code:'annotation_batch_not_accepted' });
    throw new Error('lookup uncertain');
  } });
  await test.sender.send(test.context);
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.filter(call => call.action === 'validate').length, 1, 'ambiguous lookup must not authorize admission');
  assert.equal(test.calls.filter(call => call.action === 'send' && !call.mode).length, 1);
  assert.equal(test.calls.some(call => call.action === 'acknowledge'), false);
}

{
  const gate = deferred();
  const entered = deferred();
  const test = fixture({ async send(payload) {
    entered.resolve();
    await gate.promise;
    return { queued: true, annotation_batch_id: payload.annotation_batch_id };
  } });
  const flight = test.sender.send(test.context);
  await entered.promise;
  assert.equal(test.calls.some(call => call.action === 'release'), false);
  const validation = test.calls.find(call => call.action === 'validate');
  assert.equal(validation.command.lease_id, 'batch-1');
  gate.resolve();
  assert.equal((await flight).ok, true);
  assert.equal(test.calls.find(call => call.action === 'release').command.lease_id, 'batch-1');
}

{
  const test = fixture({ native(command) {
    if (command.action === 'validate') throw new Error('validation response lost');
  } });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.some(call => call.action === 'send'), false);
  assert.equal(test.calls.at(-1).action, 'release');
  assert.equal(test.calls.at(-1).command.lease_id, 'batch-1');
}

{
  let releases = 0;
  const test = fixture({ native(command) {
    if (command.action === 'release' && ++releases === 1) throw new Error('release response lost');
  } });
  assert.equal((await test.sender.send(test.context)).ok, false);
  assert.equal(test.calls.some(call => call.action === 'acknowledge'), false);
  assert.equal((await test.sender.send(test.context)).ok, true);
  assert.equal(test.calls.filter(call => call.action === 'send').length, 1);
  assert.equal(releases, 2);
}

console.log('pageAnnotationBatches tests passed');
