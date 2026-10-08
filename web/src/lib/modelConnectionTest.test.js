import assert from 'node:assert/strict';
import { emptyModelProfileDraft } from './modelSettings.js';
import { modelTestFailureMessage, testModelDraft } from './modelConnectionTest.js';

const provider = {
  id: 'custom-openai', runtime_provider: 'openai', group: 'custom',
  auth_mode: 'required', endpoint_editable: true,
  endpoint_modes: ['base_url', 'full_url'], model_input: 'manual',
};
const draft = {
  ...emptyModelProfileDraft(), provider: 'openai', model: 'first, second',
  api_key: 'secret-key', base_url: 'https://example.com/v1',
  request_headers_json: '{"X-Test":"header-secret"}',
};

async function run(name, fn) {
  await fn();
  console.log(`[pass] ${name}`);
}

await run('model test checks every selected model sequentially with current options', async () => {
  const calls = [];
  let pending = false;
  const client = { testModel: async (payload) => {
    assert.equal(pending, false);
    pending = true;
    calls.push(payload);
    await Promise.resolve();
    pending = false;
    return { ok: true };
  } };
  await testModelDraft(client, { ...draft, name: '(unfinished)', endpoint_mode: 'full_url' }, provider);
  assert.deepEqual(calls.map((call) => call.model), ['first', 'second']);
  for (const payload of calls) {
    assert.equal(payload.api_key, 'secret-key');
    assert.equal(payload.base_url, draft.base_url);
    assert.equal(payload.endpoint_mode, 'full_url');
    assert.deepEqual(payload.request_headers, { 'X-Test': 'header-secret' });
    assert.equal(payload.original_name, undefined);
  }
});

await run('model test fails on partial success and never tests later models', async () => {
  const calls = [];
  const client = { testModel: async ({ model }) => {
    calls.push(model);
    if (model === 'second') throw { code: 'MODEL_TEST_HTTP_ERROR', body: { upstream_status: 401 } };
    return { ok: true };
  } };
  await assert.rejects(testModelDraft(client, { ...draft, model: 'first,second,third' }, provider), (error) => {
    assert.equal(error.model, 'second');
    assert.equal(error.upstreamStatus, 401);
    assert.match(modelTestFailureMessage(error, draft), /second.*HTTP 401/);
    return true;
  });
  assert.deepEqual(calls, ['first', 'second']);
});

await run('model test rejects invalid and non-success responses', async () => {
  for (const response of [null, {}, { ok: false }, { ok: 'true' }, '<html>']) {
    await assert.rejects(testModelDraft({ testModel: async () => response }, draft, provider), {
      code: 'MODEL_TEST_FAILED', model: 'first',
    });
  }
});

await run('invalid model test drafts do not send a request', async () => {
  const client = { testModel: () => assert.fail('unexpected upstream test') };
  for (const patch of [{ model: '' }, { api_key: '' }, { base_url: '' }, { request_headers_json: '{' }]) {
    await assert.rejects(testModelDraft(client, { ...draft, ...patch }, provider));
  }
});

// 「复用已有凭据」已删除:新增时没有 API Key 就不发请求(INVALID_API_KEY);
// 编辑时沿用已保存密钥的场景仍要把 original_name 带给后端且不发 api_key。
await run('model test rejects missing new-profile keys and supports retained edit credentials', async () => {
  const calls = [];
  const client = { testModel: async (payload) => { calls.push(payload); return { ok: true }; } };
  await assert.rejects(
    testModelDraft(client, { ...draft, model: 'first', api_key: '' }, provider),
    { code: 'INVALID_API_KEY' },
  );
  assert.equal(calls.length, 0);
  await testModelDraft(client, { ...draft, model: 'first', api_key: '', has_api_key: true }, provider,
    { editing: true, originalName: 'saved' });
  assert.equal(calls[0].original_name, 'saved');
  assert.equal(calls[0].api_key, undefined);
  assert.equal(Object.hasOwn(calls[0], 'credential_source_name'), false);
});

await run('aborted model tests ignore late success or failure and stop the batch', async () => {
  for (const fails of [false, true]) {
    const controller = new AbortController();
    let calls = 0;
    const client = { testModel: async (payload, { signal }) => {
      calls += 1;
      assert.equal(signal, controller.signal);
      controller.abort();
      if (fails) throw new Error('late failure');
      return { ok: true };
    } };
    await assert.rejects(testModelDraft(client, draft, provider, { signal: controller.signal }), { name: 'AbortError' });
    assert.equal(calls, 1);
    await assert.rejects(testModelDraft(client, draft, provider, { signal: controller.signal }), { name: 'AbortError' });
    assert.equal(calls, 1);
  }
});

await run('model failure copy does not echo upstream error bodies or draft secrets', async () => {
  const message = modelTestFailureMessage({
    code: 'UNKNOWN_CODE', model: 'secret-key header-secret',
    message: 'upstream-secret', body: { message: 'upstream-secret' },
  }, draft);
  assert.ok(!message.includes('secret'));
  assert.match(message, /模型检测失败/);
});

await run('model test carries the selected Responses protocol to every request', async () => {
  const calls = [];
  await testModelDraft({ testModel: async (payload) => {
    calls.push(payload);
    return { ok: true };
  } }, { ...draft, api_protocol: 'responses' }, provider);
  assert.equal(calls.length, 2);
  assert.ok(calls.every((payload) => payload.api_protocol === 'responses'));
});
