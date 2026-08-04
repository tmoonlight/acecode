import assert from 'node:assert/strict';
import { createApi } from './api.js';
import { createGitInfoCache, refreshWorkspaceGitInfo } from './gitInfoCache.js';
import { SESSION_HOVER_GIT_CACHE_TTL_MS } from './sessionHoverDetails.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function clientWithGitInfo(base, loadGitInfo) {
  const client = createApi(base);
  client.gitInfo = loadGitInfo;
  return client;
}

await test('same effective connection shares in-flight and fresh Git info across clients', async () => {
  let timestamp = 1_000;
  let calls = 0;
  let releaseFirst;
  const firstClient = clientWithGitInfo(
    { origin: 'http://daemon-a.test', token: 'same-token' },
    async () => {
      calls += 1;
      if (calls === 1) {
        return new Promise((resolve) => {
          releaseFirst = () => resolve({ is_repo: true, branch: 'shared' });
        });
      }
      return { is_repo: true, branch: 'reloaded' };
    },
  );
  const secondClient = clientWithGitInfo(
    { origin: 'http://daemon-a.test', token: 'same-token' },
    async () => {
      calls += 1;
      return { is_repo: true, branch: 'second-client' };
    },
  );
  const cache = createGitInfoCache({ now: () => timestamp });

  const first = cache.get(firstClient, 'C:/repo');
  const second = cache.get(secondClient, 'C:/repo');
  assert.strictEqual(first, second);
  await Promise.resolve();
  assert.equal(calls, 1);

  releaseFirst();
  assert.deepEqual(await Promise.all([first, second]), [
    { is_repo: true, branch: 'shared' },
    { is_repo: true, branch: 'shared' },
  ]);
  assert.equal((await cache.get(secondClient, 'C:/repo')).branch, 'shared');
  assert.equal(calls, 1);

  timestamp += SESSION_HOVER_GIT_CACHE_TTL_MS + 1;
  assert.equal((await cache.get(firstClient, 'C:/repo')).branch, 'reloaded');
  assert.equal(calls, 2);
});

await test('peek exposes only a fresh completed value synchronously', async () => {
  let timestamp = 2_000;
  let calls = 0;
  const client = clientWithGitInfo(
    { origin: 'http://daemon.test', token: 'token' },
    async () => ({ is_repo: true, branch: `branch-${++calls}` }),
  );
  const cache = createGitInfoCache({ now: () => timestamp });

  const pending = cache.get(client, 'C:/repo');
  assert.equal(cache.peek(client, 'C:/repo'), undefined);
  const loaded = await pending;
  assert.strictEqual(cache.peek(client, 'C:/repo'), loaded);

  timestamp += SESSION_HOVER_GIT_CACHE_TTL_MS;
  assert.strictEqual(cache.peek(client, 'C:/repo'), loaded);
  timestamp += 1;
  assert.equal(cache.peek(client, 'C:/repo'), undefined);
});

await test('refresh bypasses a completed value, joins in-flight work, and restarts the TTL', async () => {
  let timestamp = 3_000;
  let calls = 0;
  let releaseRefresh;
  const client = clientWithGitInfo(
    { origin: 'http://daemon.test', token: 'token' },
    async () => {
      calls += 1;
      if (calls === 1) return { is_repo: true, branch: 'cached' };
      if (calls === 2) {
        return new Promise((resolve) => {
          releaseRefresh = () => resolve({ is_repo: true, branch: 'refreshed' });
        });
      }
      return { is_repo: true, branch: 'after-expiry' };
    },
  );
  const cache = createGitInfoCache({ now: () => timestamp });

  await cache.get(client, 'C:/repo');
  timestamp = 8_000;
  const refresh = cache.refresh(client, 'C:/repo');
  const joined = cache.refresh(client, 'C:/repo');
  assert.strictEqual(joined, refresh);
  assert.equal(calls, 2);
  assert.equal(cache.peek(client, 'C:/repo').branch, 'cached');

  timestamp = 12_000;
  releaseRefresh();
  assert.equal((await refresh).branch, 'refreshed');
  assert.equal((await cache.get(client, 'C:/repo')).branch, 'refreshed');
  assert.equal(calls, 2);

  timestamp += SESSION_HOVER_GIT_CACHE_TTL_MS + 1;
  assert.equal((await cache.get(client, 'C:/repo')).branch, 'after-expiry');
  assert.equal(calls, 3);
});

await test('workspace refresh skips private or empty contexts and warms a real workspace', async () => {
  let calls = 0;
  const client = clientWithGitInfo(
    { origin: 'http://workspace-refresh.test', token: 'token' },
    async (cwd) => {
      calls += 1;
      return { is_repo: true, branch: cwd };
    },
  );

  assert.equal(await refreshWorkspaceGitInfo(client, { noWorkspace: true, cwd: 'C:/private' }), null);
  assert.equal(await refreshWorkspaceGitInfo(client, { cwd: '   ' }), null);
  assert.equal(calls, 0);
  assert.equal(
    (await refreshWorkspaceGitInfo(client, { cwd: 'C:/repo' })).branch,
    'C:/repo',
  );
  assert.equal(calls, 1);
});

await test('same cwd is isolated between daemon origins', async () => {
  let calls = 0;
  const cache = createGitInfoCache();
  const first = clientWithGitInfo(
    { origin: 'http://daemon-a.test', token: 'token' },
    async () => ({ is_repo: true, branch: `daemon-a-${++calls}` }),
  );
  const second = clientWithGitInfo(
    { origin: 'http://daemon-b.test', token: 'token' },
    async () => ({ is_repo: true, branch: `daemon-b-${++calls}` }),
  );
  const [a, b] = await Promise.all([
    cache.get(first, 'C:/same-repo'),
    cache.get(second, 'C:/same-repo'),
  ]);

  assert.match(a.branch, /^daemon-a-/);
  assert.match(b.branch, /^daemon-b-/);
  assert.equal(calls, 2);
});

await test('same origin and cwd are isolated between authentication tokens', async () => {
  let calls = 0;
  const cache = createGitInfoCache();
  const first = clientWithGitInfo(
    { origin: 'http://daemon.test', token: 'token-one' },
    async () => { calls += 1; return { is_repo: true, branch: 'token-one' }; },
  );
  const second = clientWithGitInfo(
    { origin: 'http://daemon.test', token: 'token-two' },
    async () => { calls += 1; return { is_repo: true, branch: 'token-two' }; },
  );
  const [one, two] = await Promise.all([
    cache.get(first, 'C:/same-repo'),
    cache.get(second, 'C:/same-repo'),
  ]);

  assert.equal(one.branch, 'token-one');
  assert.equal(two.branch, 'token-two');
  assert.equal(calls, 2);
});

await test('a mutable API base changes scope and old scope reloads through its caller', async () => {
  let origin = 'http://daemon-old.test';
  let token = 'old-token';
  let calls = 0;
  const mutableBase = {
    get origin() { return origin; },
    get token() { return token; },
  };
  const mutableClient = clientWithGitInfo(mutableBase, async () => {
    calls += 1;
    return { is_repo: true, branch: `${origin}|${token}` };
  });
  const cache = createGitInfoCache();

  const oldRequest = cache.get(mutableClient, 'C:/repo');
  origin = 'http://daemon-new.test';
  token = 'new-token';
  assert.equal((await oldRequest).branch, 'http://daemon-old.test|old-token');
  assert.equal(
    (await cache.get(mutableClient, 'C:/repo')).branch,
    'http://daemon-new.test|new-token',
  );

  const oldClient = clientWithGitInfo(
    { origin: 'http://daemon-old.test', token: 'old-token' },
    async () => {
      calls += 1;
      return { is_repo: true, branch: 'old-client-reload' };
    },
  );
  cache.invalidate(oldClient, 'C:/repo');
  assert.equal((await cache.get(oldClient, 'C:/repo')).branch, 'old-client-reload');
  assert.equal(calls, 3);
});

await test('scoped and global invalidation evict only the intended entries', async () => {
  let calls = 0;
  const cache = createGitInfoCache();
  const load = async (cwd) => ({ is_repo: true, branch: `${cwd}-${++calls}` });
  const first = clientWithGitInfo(
    { origin: 'http://daemon-a.test', token: 'token' },
    load,
  );
  const firstAlias = clientWithGitInfo(
    { origin: 'http://daemon-a.test', token: 'token' },
    load,
  );
  const second = clientWithGitInfo(
    { origin: 'http://daemon-b.test', token: 'token' },
    load,
  );

  await Promise.all([cache.get(first, '/repo'), cache.get(second, '/repo')]);
  assert.equal(calls, 2);

  cache.invalidate(firstAlias, '/repo');
  await cache.get(first, '/repo');
  await cache.get(second, '/repo');
  assert.equal(calls, 3);

  cache.invalidateAll('/repo');
  await Promise.all([cache.get(firstAlias, '/repo'), cache.get(second, '/repo')]);
  assert.equal(calls, 5);
});

await test('failed Git info loads are evicted and can be retried', async () => {
  let calls = 0;
  const client = clientWithGitInfo(
    { origin: 'http://daemon.test', token: 'token' },
    async () => {
      calls += 1;
      if (calls === 1) throw new Error('temporary failure');
      return { is_repo: false };
    },
  );
  const cache = createGitInfoCache();
  await assert.rejects(cache.get(client, '/repo'), /temporary failure/);
  assert.deepEqual(await cache.get(client, '/repo'), { is_repo: false });
  assert.equal(calls, 2);
});

console.log('gitInfoCache.test.js: all tests passed');
