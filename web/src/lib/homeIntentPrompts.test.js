import assert from 'node:assert/strict';
import {
  DEFAULT_HOME_INTENT_PROMPTS, HOME_INTENT_CATEGORIES, HOME_INTENT_REFRESH_MS,
  createHomeIntentController, homeIntentErrorMessage, homeIntentPromptRef,
  isHomeIntentRefreshShortcut, normalizeHomeIntentPrompts,
} from './homeIntentPrompts.js';
import { homeWorkspaceOptionForHash } from './homeWorkspaceSelection.js';

const generated = {
  category: 'execution', title: '完成任务', description: '验证剩余修改', prompt: '请完成剩余任务并验证。',
  workspace: { hash: 'other', cwd: 'N:/other', name: 'Other', noWorkspace: false },
  source_session_id: 'source',
};
async function test(name, fn) { await fn(); console.log(`[pass] home intent: ${name}`); }
function fixture() {
  let time = 1000;
  const timers = new Map();
  const calls = [];
  const errors = [];
  let timerId = 0;
  const response = () => ({ suggestions: [generated], next_refresh_at_ms: time + HOME_INTENT_REFRESH_MS });
  const api = {
    async getHomeIntentPrompts() { calls.push('read'); return { suggestions: [], next_refresh_at_ms: 0 }; },
    async refreshHomeIntentPrompts(force) { calls.push(force ? 'force' : 'refresh'); return response(); },
  };
  const controller = createHomeIntentController({ api, now: () => time, onError: (error) => errors.push(error),
    setTimer: (callback, delay) => { const id = ++timerId; timers.set(id, { callback, at: time + delay }); return id; },
    clearTimer: (id) => timers.delete(id),
  });
  return { api, controller, calls, errors, timers, response,
    advance(ms) { time += ms; },
    async flush() { for (let i = 0; i < 15; i += 1) await Promise.resolve(); },
    async tick(ms = 0) {
      time += ms;
      for (const [id, timer] of [...timers]) if (timer.at <= time) { timers.delete(id); timer.callback(); }
      for (let i = 0; i < 15; i += 1) await Promise.resolve();
    },
  };
}

await test('four editable defaults and the original SVG mapping', () => {
  assert.equal(DEFAULT_HOME_INTENT_PROMPTS.length, 4);
  assert.deepEqual(DEFAULT_HOME_INTENT_PROMPTS.map((x) => HOME_INTENT_CATEGORIES[x.category].icon),
    ['edit', 'searchSparkle', 'run', 'lightbulb']);
  assert.ok(DEFAULT_HOME_INTENT_PROMPTS.every((item) => item.prompt.length > 30));
});

// 场景：模型建议生成失败时首页展示默认卡片。
// 期望：每张默认卡片都有标题下方的灰色说明。
// 回归：说明字段曾被删掉，首页卡片只剩粗体标题。
await test('default cards keep a one-line description under the title', () => {
  for (const item of DEFAULT_HOME_INTENT_PROMPTS) {
    assert.equal(typeof item.description, 'string');
    assert.ok(item.description.trim().length > 0, item.title);
    assert.notEqual(item.description, item.title);
  }
});

// 场景：手动按 Ctrl+Y 刷新失败，失败可能来自服务端错误码、请求超时或旧版 daemon 没有该接口。
// 期望：每种原因给出能指导下一步的提示；只有模型请求失败时才让用户检查模型。
// 回归：曾经所有失败都提示「请检查模型配置」，实际多为超时或 daemon 过旧(404)。
await test('failure messages follow the error code', () => {
  const messages = ['TIMEOUT', 'MODEL_UNAVAILABLE', 'GENERATION_FAILED', 'INVALID_RESPONSE']
    .map((code) => homeIntentErrorMessage(code));
  assert.equal(new Set(messages).size, 4);
  assert.ok(messages[0].includes('超时'));
  assert.ok(messages[1].includes('摘要生成'));
  assert.equal(homeIntentErrorMessage({ status: 408, code: 'TIMEOUT' }), messages[0]);
  const stale = homeIntentErrorMessage({ status: 404, message: 'HTTP 404: 404 Not Found' });
  assert.ok(stale.includes('重启'));
  const unknown = homeIntentErrorMessage(new Error('offline'));
  assert.ok(!messages.includes(unknown) && unknown !== stale);
  assert.equal(homeIntentErrorMessage('__proto__'), unknown);
  assert.ok(messages.slice(0, 2).concat(stale, unknown).every((text) => !text.includes('检查')));
});

await test('validates generated categories and explicit workspace identity', () => {
  assert.equal(normalizeHomeIntentPrompts([generated]).length, 1);
  assert.equal(normalizeHomeIntentPrompts([{ ...generated, category: '__proto__' }]).length, 0);
  assert.equal(normalizeHomeIntentPrompts([{ ...generated, workspace: { cwd: 'N:/guess' } }]).length, 0);
  assert.equal(normalizeHomeIntentPrompts(Array(5).fill(generated)).length, 0);
  const [noWorkspace] = normalizeHomeIntentPrompts([{ ...generated,
    workspace: { noWorkspace: true, hash: 'stale', cwd: 'N:/cache/private' } }]);
  assert.equal(noWorkspace.workspace.cwd, '');
  assert.equal(noWorkspace.workspace.hash, '');
});

await test('prefill keeps workspace source and hidden workspace before list hydration', () => {
  const current = { workspaceHash: 'old', cwd: 'N:/old', port: 1234, token: 'local-test' };
  const next = homeIntentPromptRef(generated, null, current, {});
  assert.equal(next.workspaceHash, 'other');
  assert.equal(next.cwd, 'N:/other');
  assert.equal(next.initialHomePrompt, generated.prompt);
  assert.equal(next.sessionId, undefined);
  assert.equal(homeWorkspaceOptionForHash([], 'other', generated.workspace).cwd, 'N:/other');
  const empty = homeIntentPromptRef({ ...generated, workspace: { noWorkspace: true } }, null, current, {});
  assert.equal(empty.noWorkspace, true);
  assert.equal(empty.cwd, '');
  assert.equal(empty.workspaceHash, '');
  const preset = homeIntentPromptRef(DEFAULT_HOME_INTENT_PROMPTS[0], generated.workspace, current, {});
  assert.equal(preset.workspaceHash, 'other');
});

await test('only Ctrl+Y triggers debug refresh', () => {
  assert.equal(isHomeIntentRefreshShortcut({ ctrlKey: true, key: 'Y' }), true);
  for (const extra of [{ altKey: true }, { shiftKey: true }, { metaKey: true }, { isComposing: true }])
    assert.equal(isHomeIntentRefreshShortcut({ ctrlKey: true, key: 'y', ...extra }), false);
  assert.equal(isHomeIntentRefreshShortcut({ key: 'y' }), false);
});

await test('loads cache then refreshes every half hour without homepage dependence', async () => {
  const f = fixture();
  await f.controller.start();
  await f.tick();
  assert.deepEqual(f.calls, ['read', 'refresh']);
  await f.tick(HOME_INTENT_REFRESH_MS - 1);
  assert.equal(f.calls.length, 2);
  await f.tick(1);
  assert.equal(f.calls.at(-1), 'refresh');
  assert.equal(f.calls.length, 3);
  f.controller.dispose();
  assert.equal(f.timers.size, 0);
});

await test('manual refresh is immediate and concurrent presses deduplicate', async () => {
  const f = fixture(); let resolve;
  f.api.refreshHomeIntentPrompts = (force) => { f.calls.push(force); return new Promise((r) => { resolve = r; }); };
  const first = f.controller.refresh(); await f.flush();
  const second = f.controller.refresh();
  assert.equal(first, second);
  resolve(f.response()); await first; await f.flush();
  assert.deepEqual(f.calls, [true]);
  f.controller.dispose();
});

await test('sleep wake catches up once and errors preserve earlier cards', async () => {
  const f = fixture(); await f.controller.refresh();
  f.advance(HOME_INTENT_REFRESH_MS * 4);
  await f.controller.wake();
  assert.equal(f.calls.length, 2);
  f.api.refreshHomeIntentPrompts = async () => { throw new Error('offline'); };
  await f.controller.refresh();
  assert.equal(f.controller.snapshot().suggestions[0].prompt, generated.prompt);
  assert.deepEqual(f.errors.map((error) => error.message), ['offline']);
  await f.controller.wake();
  assert.equal(f.errors.length, 1);
  f.controller.dispose();
});

await test('multi-window generation polls GET without launching another model request', async () => {
  const f = fixture();
  f.api.refreshHomeIntentPrompts = async () => { f.calls.push('force'); return { ...f.response(), refreshing: true }; };
  f.api.getHomeIntentPrompts = async () => { f.calls.push('read'); return f.response(); };
  await f.controller.refresh();
  await f.tick(1500);
  assert.deepEqual(f.calls, ['force', 'read']);
  f.controller.dispose();
});

await test('dispose aborts reads and ignores their late results', async () => {
  const f = fixture(); let resolve; let signal;
  f.api.getHomeIntentPrompts = (options) => { signal = options.signal; return new Promise((r) => { resolve = r; }); };
  const pending = f.controller.start(); await f.flush();
  f.controller.dispose(); assert.equal(signal.aborted, true);
  resolve(f.response()); await pending;
  assert.equal(f.controller.snapshot().suggestions.length, 0);
  assert.equal(f.timers.size, 0);
});

// 场景：另一个窗口发起的生成一直没结束，本窗口手动刷新后持续轮询。
// 期望：服务端两段预算(找会话 30 秒 + 模型 60 秒)之内不判超时；超过 120 秒后
// 上报带 TIMEOUT 码的错误，提示文案与服务端超时一致。
await test('manual refresh polling past the limit reports a timeout code', async () => {
  const f = fixture();
  f.api.refreshHomeIntentPrompts = async () => ({ ...f.response(), refreshing: true });
  f.api.getHomeIntentPrompts = async () => ({ ...f.response(), refreshing: true });
  await f.controller.refresh();
  for (let i = 0; i < 62; i += 1) await f.tick(1500);
  assert.equal(f.errors.length, 0);
  for (let i = 0; i < 30 && !f.errors.length; i += 1) await f.tick(1500);
  assert.equal(f.errors.length, 1);
  assert.equal(homeIntentErrorMessage(f.errors[0]), homeIntentErrorMessage('TIMEOUT'));
  f.controller.dispose();
});

await test('manual refresh reports a failure delivered by another windows pending request', async () => {
  const f = fixture();
  f.api.refreshHomeIntentPrompts = async () => ({ ...f.response(), refreshing: true });
  f.api.getHomeIntentPrompts = async () => ({ ...f.response(), error: 'INVALID_RESPONSE' });
  await f.controller.refresh();
  await f.tick(1500);
  assert.deepEqual(f.errors, ['INVALID_RESPONSE']);
  f.controller.dispose();
});
