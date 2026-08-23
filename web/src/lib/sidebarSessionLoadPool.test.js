import assert from 'node:assert/strict';
import {
  createSidebarSessionLoadPool,
  sidebarSessionLoadKey,
  SIDEBAR_SESSION_LOAD_CONCURRENCY,
  SIDEBAR_SESSION_PENDING_SETTLE_MS,
} from './sidebarSessionLoadPool.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function createDeferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function createClock() {
  let current = 0;
  let nextId = 0;
  const timers = new Map();
  const runDue = () => {
    let ran = true;
    while (ran) {
      ran = false;
      const due = Array.from(timers.entries())
        .filter(([, timer]) => timer.at <= current)
        .sort((a, b) => a[1].at - b[1].at || a[0] - b[0]);
      if (due.length > 0) {
        const [id, timer] = due[0];
        timers.delete(id);
        timer.fn();
        ran = true;
      }
    }
  };
  return {
    now: () => current,
    setTimer(fn, delay) {
      nextId += 1;
      timers.set(nextId, { fn, at: current + Math.max(0, Number(delay) || 0) });
      return nextId;
    },
    clearTimer(id) {
      timers.delete(id);
    },
    advance(ms) {
      current += ms;
      runDue();
    },
  };
}

async function flushPromises() {
  await Promise.resolve();
  await Promise.resolve();
}

await run('会话加载键区分 workspace、no-workspace 和 session', () => {
  assert.equal(sidebarSessionLoadKey({ workspaceHash: 'w1', sessionId: 's1' }), 'w1:s1');
  assert.equal(sidebarSessionLoadKey({ noWorkspace: true, sessionId: 's1' }), 'no-workspace:s1');
  assert.equal(sidebarSessionLoadKey({ sessionId: '' }), '');
});

await run('前三个任务立即运行且第四个只进入 latest pending', async () => {
  assert.equal(SIDEBAR_SESSION_LOAD_CONCURRENCY, 3);
  const deferred = new Map();
  const started = [];
  const pool = createSidebarSessionLoadPool();
  const request = (key) => pool.request(key, () => {
    started.push(key);
    const value = createDeferred();
    deferred.set(key, value);
    return value.promise;
  });

  request('1');
  request('2');
  request('3');
  const fourth = request('4');

  assert.deepEqual(started, ['1', '2', '3']);
  assert.deepEqual(pool.snapshot().runningKeys, ['1', '2', '3']);
  assert.equal(pool.snapshot().pendingKey, '4');
  deferred.get('1').resolve('one');
  await flushPromises();
  assert.deepEqual(started, ['1', '2', '3']);
  assert.equal(pool.snapshot().pendingKey, '4');

  pool.dispose();
  assert.equal((await fourth).status, 'superseded');
});

await run('满载时 4→5→6→7 只保留 7 并释放旧 pending', async () => {
  const blockers = [createDeferred(), createDeferred(), createDeferred()];
  const pool = createSidebarSessionLoadPool();
  blockers.forEach((blocker, index) => {
    pool.request(String(index + 1), () => blocker.promise);
  });

  const four = pool.request('4', async () => 'four');
  const five = pool.request('5', async () => 'five');
  const six = pool.request('6', async () => 'six');
  const seven = pool.request('7', async () => 'seven');

  assert.equal((await four).status, 'superseded');
  assert.equal((await five).status, 'superseded');
  assert.equal((await six).status, 'superseded');
  assert.equal(pool.snapshot().pendingKey, '7');
  pool.dispose();
  assert.equal((await seven).status, 'superseded');
  blockers.forEach((blocker) => blocker.resolve());
});

await run('空闲槽位等待 latest pending 稳定 100ms 后才启动', async () => {
  assert.equal(SIDEBAR_SESSION_PENDING_SETTLE_MS, 100);
  const clock = createClock();
  const blockers = [createDeferred(), createDeferred(), createDeferred()];
  const started = [];
  const pool = createSidebarSessionLoadPool({
    now: clock.now,
    setTimer: clock.setTimer,
    clearTimer: clock.clearTimer,
  });
  blockers.forEach((blocker, index) => {
    pool.request(String(index + 1), () => blocker.promise);
  });
  const pending = pool.request('4', async () => {
    started.push('4');
    return 'four';
  });

  clock.advance(40);
  blockers[0].resolve('one');
  await flushPromises();
  assert.deepEqual(started, []);
  clock.advance(59);
  assert.deepEqual(started, []);
  clock.advance(1);
  assert.deepEqual(started, ['4']);
  assert.equal((await pending).status, 'loaded');
  pool.dispose();
  blockers.slice(1).forEach((blocker) => blocker.resolve());
});

await run('稳定窗口内的新 pending 替换旧目标并重新计时', async () => {
  const clock = createClock();
  const blockers = [createDeferred(), createDeferred(), createDeferred()];
  const started = [];
  const pool = createSidebarSessionLoadPool({
    now: clock.now,
    setTimer: clock.setTimer,
    clearTimer: clock.clearTimer,
  });
  blockers.forEach((blocker, index) => pool.request(String(index + 1), () => blocker.promise));
  const four = pool.request('4', async () => { started.push('4'); return 'four'; });
  clock.advance(20);
  blockers[0].resolve('one');
  await flushPromises();
  clock.advance(50);
  const five = pool.request('5', async () => { started.push('5'); return 'five'; });
  assert.equal((await four).status, 'superseded');
  clock.advance(99);
  assert.deepEqual(started, []);
  clock.advance(1);
  assert.deepEqual(started, ['5']);
  assert.equal((await five).status, 'loaded');
  pool.dispose();
  blockers.slice(1).forEach((blocker) => blocker.resolve());
});

await run('运行中同键复用 promise，成功结果进入有界缓存', async () => {
  const blocker = createDeferred();
  let calls = 0;
  const pool = createSidebarSessionLoadPool({ cacheLimit: 2 });
  const first = pool.request('same', () => {
    calls += 1;
    return blocker.promise;
  });
  const duplicate = pool.request('same', () => {
    calls += 1;
    return Promise.resolve('duplicate');
  });
  assert.equal(first, duplicate);
  assert.equal(calls, 1);
  blocker.resolve('value');
  assert.equal((await first).value, 'value');
  await flushPromises();

  const cached = await pool.request('same', () => {
    calls += 1;
    return Promise.resolve('new');
  });
  assert.equal(cached.status, 'cached');
  assert.equal(cached.value, 'value');
  assert.equal(calls, 1);

  await pool.request('two', async () => 2);
  await flushPromises();
  await pool.request('three', async () => 3);
  await flushPromises();
  assert.deepEqual(pool.snapshot().cacheKeys, ['two', 'three']);
  pool.dispose();
});

await run('点击运行中或缓存会话会丢弃不再需要的 pending', async () => {
  const blockers = [createDeferred(), createDeferred(), createDeferred()];
  const pool = createSidebarSessionLoadPool();
  const running = blockers.map((blocker, index) => (
    pool.request(String(index + 1), () => blocker.promise)
  ));
  const pending = pool.request('4', async () => 'four');
  const reused = pool.request('2', async () => 'duplicate');
  assert.equal((await pending).status, 'superseded');
  assert.equal(pool.snapshot().pendingKey, '');
  assert.equal(reused, running[1]);
  pool.dispose();
  blockers.forEach((blocker) => blocker.resolve());
});
