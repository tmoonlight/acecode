import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createInterface } from 'node:readline';
import { once } from 'node:events';
import { createApi } from './api.js';
import { createPageAnnotationController } from './pageAnnotations.js';
import { createAnnotationBatchSender } from './pageAnnotationBatches.js';
import { createSelectionAnnotation, createSelectionContext } from './selectionChatContext.js';
import { normalizeComposerPayload } from './composerSubmission.js';

const deferred = () => {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
};

export async function runAnnotationIntegration({ evaluate, request }) {
  const executable = process.env.ACECODE_TEST_ANNOTATION_SESSION;
  if (!executable) {
    console.log('[skip] native-session/browser integration requires ACECODE_TEST_ANNOTATION_SESSION');
    return;
  }
  const child = spawn(executable, [], { windowsHide: true, stdio: ['pipe', 'pipe', 'inherit'] });
  const exited = once(child, 'exit');
  const readers = [];
  const lines = createInterface({ input: child.stdout });
  lines.on('line', line => readers.shift()?.resolve(JSON.parse(line)));
  const rejectReaders = error => {
    for (const reader of readers.splice(0)) reader.reject(error);
  };
  child.on('error', rejectReaders);
  child.on('exit', code => rejectReaders(new Error(`Native integration driver exited: ${code}`)));
  const native = fields => new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`Native driver timeout: ${fields.operation}`)), 15000);
    readers.push({ resolve: value => { clearTimeout(timer); resolve(value); },
      reject: error => { clearTimeout(timer); reject(error); } });
    child.stdin.write(`${JSON.stringify(fields)}\n`);
  });
  const calls = [];
  const leaseEvents = [];
  const captureEntered = deferred();
  const releaseCapture = deferred();
  let gateCapture = true;
  const invoke = async (pageId, command) => {
    assert.equal(pageId, 'page-integration');
    calls.push(command.action);
    let response;
    if (['capture', 'validate', 'acknowledge', 'release'].includes(command.action)) {
      response = await native({ operation: 'submission', command });
      if (command.action === 'release') {
        assert.equal(response.ok, true, response.error);
        assert.equal(response.navigation_sending, false, 'Matching release must clear the real native lease');
        leaseEvents.push('released');
      }
      if (response.ok && response.expression) {
        const validation = await evaluate(response.expression);
        if (command.action === 'validate') {
          if (command.lease_id) {
            assert.equal(response.navigation_sending, true, 'Validation must hold the real native lease');
            leaseEvents.push('acquired');
          }
          const current = await native({ operation: 'lifecycle', result: validation });
          if (validation?.valid !== true || !current.ok) {
            const released = await native({ operation: 'lifecycle', result: null, release: command.lease_id || '' });
            assert.equal(released.navigation_sending, false, 'Failed host validation must release its matching lease');
            if (command.lease_id) leaseEvents.push('validation-failed-released');
            return { ok: false, page_id: pageId, error: 'Disconnected exact target' };
          }
        } else if (validation?.valid !== true) return { ok: false, page_id: pageId, error: 'Disconnected exact target' };
      }
      if (response.ok && command.action === 'capture') {
        if (gateCapture) {
          captureEntered.resolve();
          await releaseCapture.promise;
          gateCapture = false;
        }
        const screenshot = await request('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true,
          clip: { x: 0, y: 0, width: 800, height: 1400, scale: 1 } });
        response.annotation_document_id = command.document_id;
        response.result = { fullPage: true, documentId: 'injected-capture-token', data: screenshot.data };
      }
    } else {
      response = await native({ operation: 'prepare', command });
      if (response.ok) response = await native({ operation: 'complete', sequence: response.sequence,
        result: await evaluate(response.expression) });
    }
    return { ...response, page_id: pageId };
  };
  const controller = createPageAnnotationController({ pageId: 'page-integration', invoke });
  const command = async (action, fields = {}) => {
    const result = await controller.command(action, fields);
    assert.equal(result.ok, true, `${action}: ${result.error}`);
    return result.annotations;
  };
  const click = async (x, y) => {
    for (const type of ['mousePressed', 'mouseReleased']) {
      await request('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 });
    }
  };
  const open = async () => {
    await command('start');
    await click(20, 30);
    const snapshot = await command('read');
    assert.equal(snapshot.popup, true);
    return snapshot.target.id;
  };
  const uploads = [];
  const messages = [];
  const messageBodies = [];
  const receipts = [];
  const reconciliationModes = [];
  let rejectNextMessage = '';
  const server = createServer(async (req, res) => {
    try {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      assert.equal(req.headers['x-acecode-token'], 'integration-token');
      const body = Buffer.concat(chunks).toString('utf8');
      const payload = JSON.parse(body);
      assert.equal(req.method, 'POST');
      const url = new URL(req.url, 'http://127.0.0.1');
      assert.match(url.pathname, /^\/api\/sessions\/initiating-session\/(attachments|messages)$/);
      assert.ok([...url.searchParams.keys()].every(key => key === 'annotation_reconcile_only'));
      res.setHeader('content-type', 'application/json');
      if (url.pathname.endsWith('/attachments')) {
        assert.equal(url.search, '');
        uploads.push(payload);
        res.end(JSON.stringify({ attachment: { id: 'page-image' } }));
      } else {
        messages.push(payload);
        messageBodies.push(body);
        reconciliationModes.push(url.searchParams.get('annotation_reconcile_only'));
        if (rejectNextMessage) {
          res.statusCode = 503;
          res.end(JSON.stringify({ error: rejectNextMessage }));
          rejectNextMessage = '';
          return;
        }
        const receipt = await native({ operation: 'receive', payload,
          reconciliation: url.searchParams.get('annotation_reconcile_only') || '' });
        receipts.push(receipt);
        if (!receipt.ok) {
          res.statusCode = receipt.status || 500;
          res.end(JSON.stringify({ error: receipt.error }));
          return;
        }
        res.end(JSON.stringify(messages.length === 1
          ? { queued: true, annotation_batch_id: 'wrong-receipt' } : receipt));
      }
    } catch (error) {
      res.statusCode = 500;
      res.end(JSON.stringify({ message: String(error.stack) }));
    }
  });
  try {
    await evaluate(`globalThis.__acecodePageAnnotationsV1?.dispose();
      document.body.innerHTML = '<button id="parent" style="position:absolute;left:10px;top:20px;width:180px;height:40px;color:red">Hello <strong id="nested">world</strong>!</button><div id="second" style="position:absolute;left:250px;top:20px;width:100px;height:40px;color:red">Second</div><div style="position:absolute;top:1300px">below viewport</div>';
      globalThis.parentTarget = document.getElementById('parent');
      globalThis.nestedTarget = document.getElementById('nested');
      globalThis.originalNodes = [...parentTarget.childNodes];
      globalThis.originalParentMarkup = parentTarget.outerHTML;
      globalThis.nestedClicks = 0; nestedTarget.addEventListener('identity-probe', () => nestedClicks++);`);
    const firstTarget = await open();
    await command('preview_text', { text: 'Welcome world!' });
    await command('preview_style', { styles: { color: 'blue' } });
    await command('add_target');
    await click(260, 30);
    let snapshot = await command('read');
    const secondTarget = snapshot.target.id;
    assert.notEqual(secondTarget, firstTarget);
    await command('preview_text', { text: 'Second edited' });
    await command('preview_style', { styles: { opacity: '0.5' } });
    await command('select_target', { target_id: firstTarget });
    assert.equal(controller.getSnapshot().target.text_preview.value, 'Welcome world!');
    await command('add_region');
    await request('Input.dispatchMouseEvent', { type: 'mousePressed', x: 400, y: 150, button: 'left', clickCount: 1 });
    await request('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 460, y: 190, button: 'left', buttons: 1 });
    await request('Input.dispatchMouseEvent', { type: 'mouseReleased', x: 460, y: 190, button: 'left', clickCount: 1 });
    snapshot = await command('read');
    assert.deepEqual(snapshot.region, { x: 400, y: 150, width: 60, height: 40 });
    snapshot = await command('save', { description: 'One request for both targets and region' });
    const record = snapshot.records[0];
    assert.equal(record.number, 1);
    assert.deepEqual(record.targets.map(target => target.id), [firstTarget, secondTarget]);
    assert.equal(record.differences.length, 4);
    assert.deepEqual(record.differences.filter(change => change.property === 'text').map(change =>
      [change.old_value, change.new_value]), [['Hello world!', 'Welcome world!'], ['Second', 'Second edited']]);
    assert.equal(await evaluate('parentTarget.textContent'), 'Welcome world!');
    assert.equal(await evaluate('originalNodes.every((node,index)=>parentTarget.childNodes[index]===node) && parentTarget.firstElementChild===nestedTarget'), true);
    await command('original', { enabled: true });
    assert.equal(await evaluate('parentTarget.textContent'), 'Hello world!');
    assert.equal(await evaluate('getComputedStyle(parentTarget).color'), 'rgb(255, 0, 0)');
    await command('original', { enabled: false });
    await open();
    await command('preview_text', { text: 'Discard draft' });
    await command('preview_style', { styles: { color: 'green' } });
    await command('original', { enabled: true });
    assert.equal(await evaluate('parentTarget.textContent'), 'Hello world!');
    await command('original', { enabled: false });
    assert.equal(await evaluate('parentTarget.textContent'), 'Discard draft');
    await command('cancel');
    assert.equal(await evaluate('parentTarget.textContent'), 'Welcome world!');
    assert.equal(await evaluate('getComputedStyle(parentTarget).color'), 'rgb(0, 0, 255)');
    assert.equal(await evaluate("nestedTarget.dispatchEvent(new Event('identity-probe')); nestedClicks"), 1);
    console.log('[pass] trusted browser input → production controller/native session saves actual multi-target text/style/region differences; original and cancel preserve nested identities/listeners');

    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const base = { origin: `http://127.0.0.1:${server.address().port}`, token: 'integration-token' };
    const api = createApi(base);
    const sender = createAnnotationBatchSender({ pageId: 'page-integration', invoke, makeId: () => 'integration-batch' });
    const context = { snapshot: controller.getSnapshot(), sessionId: 'initiating-session', api,
      page: { url: 'https://fixture.invalid/long-page', title: 'Frozen title' } };
    const flight = sender.send(context);
    await Promise.race([captureEntered.promise, flight.then(result => {
      assert.fail(`Send finished before the capture gate: ${JSON.stringify(result)}`);
    })]);
    assert.equal(sender.send({ ...context, sessionId: 'other-session' }), flight);
    base.origin = 'http://127.0.0.1:1';
    base.token = 'changed-token';
    context.page.title = 'Changed after freeze';
    await open();
    await command('preview_text', { text: 'Later saved edit' });
    snapshot = await command('save', { description: 'Next batch only' });
    assert.equal(snapshot.records[1].number, 2);
    releaseCapture.resolve();
    assert.equal((await flight).ok, false);
    assert.equal((await native({ operation: 'snapshot' })).navigation_sending, false);
    assert.deepEqual(leaseEvents, ['acquired', 'released']);
    assert.equal((await native({ operation: 'snapshot' })).annotations.pending_count, 2);
    assert.equal((await sender.send({ ...context, sessionId: 'other-session' })).ok, false);
    assert.equal(messages.length, 1);
    const accepted = await sender.send({ ...context, snapshot });
    assert.equal(accepted.ok, true, accepted.error);
    assert.equal(accepted.annotations.pending_count, 1);
    assert.deepEqual(accepted.annotations.records.map(item => item.status), ['sent', 'pending']);
    assert.equal(uploads.length, 1);
    assert.equal(uploads[0].mime_type, 'image/png');
    const png = Buffer.from(uploads[0].data_base64, 'base64');
    assert.equal(png.subarray(1, 4).toString(), 'PNG');
    assert.equal(png.readUInt32BE(20), 1400);
    assert.equal(calls.filter(action => action === 'capture').length, 1);
    assert.equal(calls.filter(action => action === 'validate').length, 1);
    assert.equal(calls.filter(action => action === 'acknowledge').length, 1);
    assert.equal(calls.filter(action => action === 'release').length, 1);
    assert.deepEqual(messages[1], messages[0]);
    assert.equal(messageBodies[1], messageBodies[0]);
    assert.deepEqual(reconciliationModes, [null, '1']);
    assert.equal(receipts[0].duplicate, false);
    assert.equal(receipts[1].duplicate, true);
    const formatted = JSON.parse(messages[0].text.slice('Page annotations\n'.length));
    assert.equal(formatted.page.title, 'Frozen title');
    assert.equal(formatted.annotations.length, 1);
    assert.deepEqual(formatted.annotations[0].differences, record.differences);
    assert.deepEqual(messages[0].attachments, [{ id: 'page-image' }]);

    const source = createSelectionContext({ path: 'src/example.cpp', kind: 'code', text: 'original source',
      startLine: 3, endLine: 3, annotations: [createSelectionAnnotation({ text: 'Keep source workflow', now: 123 })] });
    const composer = normalizeComposerPayload('Unsent composer draft', [{ id: 'existing-attachment', name: 'notes.txt' }], [source]);
    await createApi({ origin: `http://127.0.0.1:${server.address().port}`, token: 'integration-token' })
      .sendInput('initiating-session', composer);
    console.log('[pass] saved records → frozen batch → real API HTTP serialization → native parser/busy ordinary queue dedup → matching receipt marks only frozen IDs; source/composer payload remains independently sendable');

    const pendingRecord = accepted.annotations.records[1];
    const validation = { action: 'validate', document_id: snapshot.document_id, record_ids: [pendingRecord.id] };
    assert.equal((await invoke('page-integration', { ...validation, document_id: 'foreign-document' })).ok, false);
    assert.equal((await invoke('page-integration', { ...validation, record_ids: ['unowned-record'] })).ok, false);
    assert.equal((await native({ operation: 'prepare', command: { action: 'read', document_id: snapshot.document_id,
      expression: 'fetch("https://attacker.invalid")' } })).ok, false);
    const retrySender = createAnnotationBatchSender({ pageId: 'page-integration', invoke, makeId: () => 'rejected-batch' });
    const retryContext = { snapshot: accepted.annotations, sessionId: 'initiating-session',
      api: createApi({ origin: `http://127.0.0.1:${server.address().port}`, token: 'integration-token' }), page: context.page };
    rejectNextMessage = 'annotation_batch_unavailable';
    assert.equal((await retrySender.send(retryContext)).ok, false);
    assert.equal(messages.length, 4);
    const uncertainSender = createAnnotationBatchSender({ pageId: 'page-integration', invoke, makeId: () => 'uncertain-batch' });
    rejectNextMessage = 'injected_transport_failure_before_admission';
    assert.equal((await uncertainSender.send(retryContext)).ok, false);
    assert.equal(messages.length, 5);
    await evaluate('globalThis.impostor = parentTarget.cloneNode(true); parentTarget.replaceWith(impostor)');
    assert.equal((await invoke('page-integration', { ...validation, lease_id: 'failed-validation-probe' })).ok, false);
    assert.equal(leaseEvents.at(-1), 'validation-failed-released');
    assert.equal((await native({ operation: 'snapshot' })).navigation_sending, false);
    assert.equal((await retrySender.send(retryContext)).ok, false,
      'An explicitly rejected batch must not first-admit after its exact target is replaced');
    assert.equal(messages.length, 5, 'Explicitly rejected invalid retry must stop before another HTTP admission');
    assert.equal((await uncertainSender.send(retryContext)).ok, false);
    assert.equal(messages.length, 6, 'Uncertain invalid retry may only query the existing receipt');
    assert.equal(reconciliationModes[5], '1');
    assert.deepEqual(messages[5], messages[4], 'Receipt lookup must preserve the exact frozen payload');
    assert.equal(messageBodies[5], messageBodies[4], 'Receipt lookup must preserve the serialized HTTP body');
    assert.equal(receipts.at(-1).status, 404);
    assert.equal(receipts.at(-1).error, 'annotation_batch_not_accepted');
    assert.equal(receipts.at(-1).annotation_batch_id, '');
    assert.equal((await native({ operation: 'snapshot' })).navigation_sending, false);
    assert.equal((await native({ operation: 'snapshot' })).annotations.pending_count, 1);
    const drained = await native({ operation: 'drain' });
    assert.equal(drained.priority_count, 0);
    assert.equal(drained.ordinary.length, 2, 'Rejected invalid batch must never enter the real busy queue');
    assert.deepEqual(drained.ordinary[0], messages[0]);
    assert.deepEqual(drained.ordinary[1], composer);
    assert.equal(drained.ordinary[1].contexts[0].source.path, 'src/example.cpp');
    assert.equal(drained.ordinary[1].contexts[0].annotations[0].text, 'Keep source workflow');
    assert.equal(drained.ordinary[1].attachments[0].id, 'existing-attachment');
    console.log('[pass] explicit HTTP refusal → real exact-target replacement → retry cannot newly admit or clear pending work');
    await evaluate('impostor.replaceWith(parentTarget)');
    const prepared = await native({ operation: 'prepare', command: { action: 'read', document_id: snapshot.document_id } });
    assert.equal(prepared.ok, true);
    const staleResult = await evaluate(prepared.expression);
    const replaced = await native({ operation: 'replace' });
    assert.equal(replaced.annotations.records.every(item => item.valid === false), true);
    assert.equal((await native({ operation: 'complete', sequence: prepared.sequence, result: staleResult })).ok, false);
    assert.equal((await native({ operation: 'prepare', page_id: 'foreign-page',
      command: { action: 'start', document_id: '' } })).ok, false);
    assert.equal((await invoke('page-integration', validation)).ok, false);
    controller.observe(replaced.annotations);
    console.log('[pass] native document/page/membership binding rejects untrusted fields, cloned replacements and stale callbacks without HTTP admission');
    await open();
    assert.equal(await evaluate('parentTarget.textContent'), 'Hello world!', 'Replaced document starts with restored original text');
    const nestedBounds = await evaluate('(() => { const r=nestedTarget.getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2,width:r.width}; })()');
    assert.ok(nestedBounds.width > 0, 'Nested target must have real hit-testable text');
    await command('add_target');
    await click(nestedBounds.x, nestedBounds.y);
    snapshot = await command('read');
    const nestedId = snapshot.target.id;
    assert.notEqual(nestedId, firstTarget);
    await command('preview_text', { text: 'Earth' });
    await command('select_target', { target_id: firstTarget });
    await command('preview_text', { text: 'Hi world!' });
    assert.equal(await evaluate('parentTarget.textContent'), 'Hi Earth!',
      'Parent owns its changed prefix, not the unchanged descendant owned by the child layer');
    await command('original', { enabled: true });
    assert.equal(await evaluate('parentTarget.outerHTML'), await evaluate('originalParentMarkup'));
    await command('original', { enabled: false });
    assert.equal(await evaluate('parentTarget.textContent'), 'Hi Earth!');
    snapshot = await command('save', { description: 'Nested exact target changes' });
    const textChanges = snapshot.records.at(-1).differences.filter(change => change.property === 'text');
    assert.equal(await evaluate('parentTarget.textContent'), 'Hi Earth!');
    assert.equal(await evaluate('nestedTarget.textContent'), 'Earth');
    assert.equal(textChanges.length, 2, 'Both changed targets must have an effective saved difference');
    assert.deepEqual(textChanges.map(change => [change.target_id, change.old_value, change.new_value])
      .sort((left, right) => left[0].localeCompare(right[0])),
    [[firstTarget, 'Hello world!', 'Hi Earth!'], [nestedId, 'world', 'Earth']]
      .sort((left, right) => left[0].localeCompare(right[0])));
    assert.ok(textChanges.every(change => change.old_value !== change.new_value));
    for (const change of textChanges) {
      const actual = await evaluate(change.target_id === nestedId ? 'nestedTarget.textContent' : 'parentTarget.textContent');
      assert.equal(change.new_value, actual, `Saved text diff must describe final DOM for ${change.target_id}`);
    }
    await open();
    await command('preview_text', { text: 'Discard overlapping draft' });
    await command('cancel');
    assert.equal(await evaluate('parentTarget.textContent'), 'Hi Earth!');
    assert.equal(await evaluate('nestedTarget.textContent'), 'Earth');
    console.log('[pass] overlapping parent/child text differences describe the final DOM, not overwritten draft intentions');
    await evaluate('globalThis.__acecodePageAnnotationsV1.dispose()');
    assert.equal(await evaluate('parentTarget.outerHTML === originalParentMarkup && originalNodes.every((node,index)=>parentTarget.childNodes[index]===node)'), true);
  } finally {
    releaseCapture.resolve();
    controller.dispose();
    server.closeAllConnections();
    if (server.listening) await new Promise(resolve => server.close(resolve));
    child.stdin.end();
    await Promise.race([exited, new Promise(resolve => setTimeout(resolve, 1000))]);
    if (child.exitCode === null) child.kill();
    lines.close();
  }
}
