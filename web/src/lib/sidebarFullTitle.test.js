import assert from 'node:assert/strict';
import {
  createSidebarFullTitleLoader,
  sidebarFullTitleRequestKey,
  sidebarTitleHydrationState,
} from './sidebarFullTitle.js';

function run(name, fn) {
  try {
    const result = fn();
    if (result && typeof result.then === 'function') {
      return result.then(
        () => console.log(`[pass] ${name}`),
        (error) => { console.error(`[fail] ${name}`); throw error; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('storage-truncated fallback drops only its synthetic suffix before hydration', () => {
  assert.deepEqual(
    sidebarTitleHydrationState(
      { summary: '完整会话标题的前缀...' },
      '完整会话标题的前缀...',
    ),
    { displayTitle: '完整会话标题的前缀', needsFullTitle: true },
  );
  assert.deepEqual(
    sidebarTitleHydrationState(
      { summary: '短标题' },
      '短标题',
    ),
    { displayTitle: '短标题', needsFullTitle: false },
  );
});

run('explicit titles keep intentional trailing dots and need no hydration', () => {
  assert.deepEqual(
    sidebarTitleHydrationState(
      { title: '用户命名的标题...', summary: '旧摘要...' },
      '用户命名的标题...',
    ),
    { displayTitle: '用户命名的标题...', needsFullTitle: false },
  );
});

run('generated error titles still hydrate their truncated fallback summary', () => {
  assert.deepEqual(
    sidebarTitleHydrationState(
      {
        title: '[Error] title generation failed',
        title_source: 'generated',
        summary: '完整请求的前缀...',
      },
      '完整请求的前缀...',
    ),
    { displayTitle: '完整请求的前缀', needsFullTitle: true },
  );
});

run('full-title request identity changes with transcript content counters', () => {
  const base = {
    id: 'session-1',
    workspace_hash: 'workspace-1',
    turn_count: 2,
    message_count: 4,
    summary: '摘要...',
  };
  assert.notEqual(
    sidebarFullTitleRequestKey(base),
    sidebarFullTitleRequestKey({ ...base, turn_count: 3, message_count: 6 }),
  );
});

await run('loader fetches the complete latest user text once per workspace session revision', async () => {
  const calls = [];
  let resolveMessages;
  const messagesReady = new Promise((resolve) => { resolveMessages = resolve; });
  const api = {
    getMessages(...args) {
      calls.push(args);
      return messagesReady;
    },
  };
  const session = {
    id: 'session-1',
    workspace_hash: 'workspace/hash',
    turn_count: 2,
    message_count: 4,
    summary: '最后一条完整用户消息的前缀...',
  };
  const load = createSidebarFullTitleLoader();
  const first = load(api, session);
  const concurrent = load(api, session);

  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0], ['session-1', 0, 'workspace/hash']);
  resolveMessages({
    messages: [
      { role: 'user', content: '第一条用户消息' },
      { role: 'assistant', content: '回复' },
      { role: 'user', content: '最后一条完整用户消息，后面的所有文字都必须进入跑马灯' },
    ],
  });

  assert.equal(
    await first,
    '最后一条完整用户消息，后面的所有文字都必须进入跑马灯',
  );
  assert.equal(await concurrent, await first);
  assert.equal(await load(api, session), await first);
  assert.equal(calls.length, 1);
});

await run('failed full-title loads remain retryable', async () => {
  let attempts = 0;
  const api = {
    getMessages() {
      attempts += 1;
      if (attempts === 1) return Promise.reject(new Error('temporary failure'));
      return Promise.resolve({ messages: [{ role: 'user', content: '重试后的完整标题' }] });
    },
  };
  const session = { id: 'session-2', summary: '重试后的完整...' };
  const load = createSidebarFullTitleLoader();

  await assert.rejects(load(api, session), /temporary failure/);
  assert.equal(await load(api, session), '重试后的完整标题');
  assert.equal(attempts, 2);
});
