import assert from 'node:assert/strict';
import { createSideChatSessionStore, sideChatSessionKey } from './sideChatSessions.js';
import {
  DEFAULT_SIDE_CHAT_RETENTION_MINUTES,
  isSideChatRetentionMinutes,
  sideChatRetentionMilliseconds,
} from './sideChatRetention.js';

function run(name, fn) {
  fn();
  console.log(`[pass] side chat retention: ${name}`);
}

function setup() {
  let time = 0;
  const requests = [];
  const store = createSideChatSessionStore({ now: () => time });
  const key = (id) => sideChatSessionKey({ port: 28080, workspaceHash: 'workspace', sessionId: id });
  const get = (id) => store.get(key(id), (options) => {
    const request = { ...options, stops: 0, disposals: 0 };
    requests.push(request);
    return { stop() { request.stops++; }, dispose() { request.disposals++; } };
  });
  return { get, requests, at(value) { time = value; } };
}

run('defaults to 30 minutes and accepts positive integer minutes only', () => {
  assert.equal(DEFAULT_SIDE_CHAT_RETENTION_MINUTES, 30);
  assert.equal(sideChatRetentionMilliseconds(1), 60_000);
  assert.equal(isSideChatRetentionMinutes(240), true);
  for (const invalid of [undefined, null, '', '5', 0, -1, 1.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1]) {
    assert.equal(isSideChatRetentionMinutes(invalid), false);
    assert.equal(sideChatRetentionMilliseconds(invalid), 30 * 60_000);
  }
});

run('session identity includes daemon and workspace and excludes empty sessions', () => {
  const ref = { port: 1, workspaceHash: 'w', sessionId: 'same' };
  assert.notEqual(sideChatSessionKey(ref), sideChatSessionKey({ ...ref, port: 2 }));
  assert.notEqual(sideChatSessionKey(ref), sideChatSessionKey({ ...ref, workspaceHash: 'other' }));
  assert.equal(sideChatSessionKey(ref), sideChatSessionKey({ port: 1, workspace_hash: 'w', id: 'same' }));
  assert.equal(sideChatSessionKey({ workspaceHash: 'home' }), '');
  const store = createSideChatSessionStore();
  assert.notEqual(store.get('', () => {}), store.get('', () => {}));
});

run('switching and remounting restore open state, draft and follow-up history independently', () => {
  const { get, requests, at } = setup();
  const first = get('first');
  first.submit('first question');
  at(5_000);
  requests[0].onDone({ answer: 'first answer' });
  first.setDraft('unsent draft');
  first.suspend();
  const second = get('second');
  assert.equal(second.getSnapshot().open, false);
  assert.deepEqual(second.getSnapshot().turns, []);
  second.submit('second question');
  requests[1].onDone({ answer: 'second answer' });
  second.suspend();
  at(30 * 60_000 + 4_999);
  const restored = get('first');
  assert.equal(restored, first);
  assert.equal(restored.resume(30), false);
  assert.equal(restored.getSnapshot().open, true);
  assert.equal(restored.getSnapshot().draft, 'unsent draft');
  assert.equal(restored.getSnapshot().turns[0].answer, 'first answer');
  restored.submit();
  assert.deepEqual(requests[2].history, [
    { role: 'user', content: 'first question' },
    { role: 'assistant', content: 'first answer' },
  ]);
  assert.equal(second.getSnapshot().turns[0].answer, 'second answer');
});

run('expiry starts at completion, stays visible until return and includes the exact boundary', () => {
  const { get, requests, at } = setup();
  const chat = get('session');
  chat.submit('long answer');
  at(40 * 60_000);
  assert.equal(chat.resume(30), false, 'a generating request must not expire');
  requests[0].onDone({ answer: 'finished after forty minutes' });
  at(70 * 60_000 - 1);
  chat.suspend();
  assert.equal(get('session').resume(30), false);
  at(70 * 60_000);
  assert.equal(chat.getSnapshot().turns.length, 1, 'no background timeout clears the visible chat');
  chat.suspend();
  assert.equal(get('session').resume(30), true);
  assert.deepEqual(chat.getSnapshot(), { open: false, turns: [], draft: '', busy: false, stopping: false });
  chat.open();
  chat.submit('fresh question');
  assert.deepEqual(requests[1].history, []);
});

run('switches, reopening and editing do not extend the end time', () => {
  const { get, requests, at } = setup();
  const chat = get('session');
  chat.submit('question');
  requests[0].onDone({ answer: 'answer' });
  at(60_000);
  chat.close();
  chat.open();
  chat.setDraft('new draft');
  chat.suspend();
  chat.resume(30);
  at(30 * 60_000);
  assert.equal(chat.resume(30), true);
});

run('each new completion renews retention and active follow-ups never expire', () => {
  const { get, requests, at } = setup();
  const chat = get('session');
  chat.submit('first');
  requests[0].onDone({ answer: 'first answer' });
  at(29 * 60_000);
  chat.submit('second');
  at(45 * 60_000);
  assert.equal(chat.resume(30), false);
  requests[1].onDone({ answer: 'second answer' });
  at(75 * 60_000 - 1);
  assert.equal(chat.resume(30), false);
  at(75 * 60_000);
  assert.equal(chat.resume(30), true);
});

run('leaving a streaming session stops it and retains partial output, suppressing late callbacks', () => {
  const { get, requests, at } = setup();
  const first = get('first');
  first.submit('question');
  requests[0].onDelta('partial answer');
  requests[0].onTool({ callId: 'tool', name: 'grep', status: 'running' });
  at(40 * 60_000);
  first.suspend();
  assert.equal(requests[0].stops, 1);
  assert.equal(requests[0].disposals, 1);
  assert.equal(first.getSnapshot().turns[0].status, 'stopped');
  assert.equal(first.getSnapshot().turns[0].tools[0].status, 'cancelled');
  const second = get('second');
  const before = second.getSnapshot();
  requests[0].onDelta('late delta');
  requests[0].onReset();
  requests[0].onError({ message: 'late failure' });
  requests[0].onDone({ answer: 'late answer' });
  assert.equal(second.getSnapshot(), before);
  at(41 * 60_000);
  assert.equal(first.resume(30), false);
  assert.equal(first.getSnapshot().open, true);
  assert.equal(first.getSnapshot().turns[0].answer, 'partial answer');
  first.submit('follow-up');
  assert.equal(requests[1].history[1].content, 'partial answer');
  const fresh = first.getSnapshot();
  requests[0].onDone({ answer: 'late old result' });
  assert.equal(first.getSnapshot(), fresh);
});

for (const ending of ['success', 'error', 'stop']) {
  run(`${ending} retention begins when the request ends`, () => {
    const { get, requests, at } = setup();
    const chat = get('session');
    chat.submit('question');
    chat.stop();
    at(10 * 60_000);
    if (ending === 'error') requests[0].onError({ message: 'model error' });
    else requests[0].onDone({ answer: 'answer', cancelled: ending === 'stop' });
    at(40 * 60_000 - 1);
    assert.equal(chat.resume(30), false);
    at(40 * 60_000);
    assert.equal(chat.resume(30), true);
  });
}

run('uses the newest retention setting without rewriting the last end time', () => {
  const { get, requests, at } = setup();
  const chat = get('session');
  chat.submit('question');
  requests[0].onDone({ answer: 'answer' });
  at(35 * 60_000);
  assert.equal(chat.resume(60), false);
  assert.equal(chat.resume(10), true);
});

run('invalid submissions do not renew the completed conversation', () => {
  const { get, requests, at } = setup();
  const chat = get('session');
  chat.submit('question');
  requests[0].onDone({ answer: 'answer' });
  at(29 * 60_000);
  assert.equal(chat.submit('x'.repeat(16001)), false);
  assert.equal(requests.length, 1);
  at(30 * 60_000);
  assert.equal(chat.resume(30), true);
});

run('minimized windows stay minimized and drafts without a completed chat have no expiry', () => {
  const { get, at } = setup();
  const chat = get('draft');
  chat.open();
  chat.setDraft('not sent');
  chat.close();
  chat.suspend();
  at(100 * 60_000);
  assert.equal(get('draft').resume(1), false);
  assert.equal(chat.getSnapshot().open, false);
  assert.equal(chat.getSnapshot().draft, 'not sent');
});

run('clear resets the old clock and keeps other sessions intact', () => {
  const { get, requests, at } = setup();
  const chat = get('clear');
  chat.submit('old question');
  requests[0].onDone({ answer: 'old answer' });
  const other = get('other');
  other.setDraft('other draft');
  chat.clear();
  chat.setDraft('fresh draft');
  at(60 * 60_000);
  assert.equal(chat.resume(30), false);
  assert.equal(chat.getSnapshot().draft, 'fresh draft');
  assert.equal(other.getSnapshot().draft, 'other draft');
});

run('restoring a controller refreshes the transport delegate', () => {
  const store = createSideChatSessionStore();
  let oldCalls = 0;
  let newCalls = 0;
  const chat = store.get('same', () => { oldCalls++; });
  assert.equal(store.get('same', () => { newCalls++; }), chat);
  chat.submit('question');
  assert.equal(oldCalls, 0);
  assert.equal(newCalls, 1);
});
