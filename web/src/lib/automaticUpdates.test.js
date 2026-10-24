import assert from 'node:assert/strict';
import { createUpdateController, randomUpdateCheckDelay, automaticUpdateFailed,
  MIN_UPDATE_CHECK_DELAY_MS, MAX_UPDATE_CHECK_DELAY_MS } from './automaticUpdates.js';

const available = { status: 'available', update_available: true, latest_version: '2.0.0' };
const flush = async () => { for (let i = 0; i < 20; i += 1) await Promise.resolve(); };
const deferred = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };
function fixture(overrides = {}, options = {}) {
  let clock = 0;
  let seq = 0;
  const timers = new Map();
  const notices = [];
  const calls = { check: 0, start: 0, poll: 0 };
  const api = {
    getUpdateStatus: async () => { calls.check += 1; return available; },
    getLatestUpdateJob: async () => { throw Object.assign(new Error('missing'), { status: 404 }); },
    getUpgradeConfig: async () => ({ auto_update: false }),
    startUpdate: async ({ automatic }) => {
      calls.start += 1;
      return { job_id: `job-${calls.start}`, state: 'running', target_version: '2.0.0', automatic };
    },
    getUpdateJob: async (id) => { calls.poll += 1; return { job_id: id, state: 'running', automatic: true }; },
    ...overrides,
  };
  const controller = createUpdateController({ api, notify: (item) => notices.push(item), random: () => 0,
    setTimer: (fn, delay) => { const id = ++seq; timers.set(id, { fn, at: clock + delay, delay }); return id; },
    clearTimer: (id) => timers.delete(id),
    ...options,
  });
  return { api, controller, calls, notices, timers,
    async advance(ms) {
      const end = clock + ms;
      for (;;) {
        const next = [...timers].sort((a, b) => a[1].at - b[1].at)[0];
        if (!next || next[1].at > end) break;
        clock = next[1].at; timers.delete(next[0]); next[1].fn(); await flush();
      }
      clock = end; await flush();
    },
  };
}
async function test(name, fn) { await fn(); console.log(`[pass] ${name}`); }

await test('automatic update interval samples both inclusive boundaries each cycle', async () => {
  assert.equal(randomUpdateCheckDelay(() => 0), 600000);
  assert.equal(randomUpdateCheckDelay(() => 1), 7200000);
  assert.equal(randomUpdateCheckDelay(() => 0.5), 3900000);
  assert.equal(randomUpdateCheckDelay(() => NaN), MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(randomUpdateCheckDelay(() => 2), MAX_UPDATE_CHECK_DELAY_MS);
  const f = fixture(); await f.controller.start();
  assert.equal(f.calls.check, 1);
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS - 1); assert.equal(f.calls.check, 1);
  await f.advance(1); assert.equal(f.calls.check, 2);
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS); assert.equal(f.calls.check, 3);
  assert.equal(f.calls.start, 0); assert.deepEqual(f.notices, []);
  assert.equal(f.controller.getSnapshot().dialogOpen, false);
  f.controller.stop(); assert.equal(f.timers.size, 0);
});

await test('failed automatic checks preserve the last offer and never expose errors', async () => {
  const f = fixture(); await f.controller.start();
  f.api.getUpdateStatus = async () => { throw new Error('offline'); };
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(f.controller.getSnapshot().status, available);
  assert.equal(f.controller.getSnapshot().dialogOpen, false);
  assert.equal(f.controller.getSnapshot().checking, false);
  f.api.getUpdateStatus = async () => ({ status: 'manifest_invalid', error: 'bad json' });
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(f.controller.getSnapshot().status, available);
  assert.equal(automaticUpdateFailed(available, f.controller.getSnapshot().job), false);
  assert.deepEqual(f.notices, []); f.controller.stop();
});

await test('automatic success uses the requested toast, stays closed and waits for restart', async () => {
  const f = fixture({ getUpgradeConfig: async () => ({ auto_update: true }),
    getUpdateJob: async (id) => ({ job_id: id, state: 'succeeded', automatic: true,
      target_version: '2.0.0', restart_required: true, automatic_failures: 0 }),
  });
  await f.controller.start(); await flush();
  assert.equal(f.calls.start, 1);
  assert.deepEqual(f.notices, [{ kind: 'ok', text: '更新已完成，重启后生效' }]);
  assert.equal(f.controller.getSnapshot().dialogOpen, false);
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS * 3);
  assert.equal(f.calls.start, 1); assert.equal(f.notices.length, 1);
  f.controller.stop();
});

await test('three distinct automatic failures show a retryable failure without a toast', async () => {
  let attempts = 0;
  const f = fixture({ getUpgradeConfig: async () => ({ auto_update: true }),
    getUpdateJob: async (id) => ({ job_id: id, state: 'failed', automatic: true,
      target_version: '2.0.0', automatic_failures: ++attempts, error: 'download failed' }),
  });
  await f.controller.start(); await flush();
  assert.equal(automaticUpdateFailed(available, f.controller.getSnapshot().job), false);
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(automaticUpdateFailed(available, f.controller.getSnapshot().job), false);
  await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(automaticUpdateFailed(available, f.controller.getSnapshot().job), true);
  assert.equal(f.controller.getSnapshot().dialogOpen, false);
  assert.deepEqual(f.notices, []);
  f.controller.openDialog(); assert.equal(f.controller.getSnapshot().dialogOpen, true);
  await f.controller.startUpdate(); assert.equal(f.calls.start, 4);
  assert.equal(automaticUpdateFailed({ ...available, latest_version: '3.0.0' }, f.controller.getSnapshot().job), false);
  f.controller.stop();
});

await test('progress failures keep the actual job running and recover silently with bounded backoff', async () => {
  const job = { job_id: 'recover', state: 'running', automatic: true, target_version: '2.0.0', automatic_failures: 2 };
  const f = fixture({ getLatestUpdateJob: async () => job,
    getUpdateJob: async () => { throw new Error('network'); }, getUpgradeConfig: async () => ({ auto_update: true }),
  });
  await f.controller.start(); await flush();
  await f.advance(5 * 60 * 1000);
  assert.equal(f.controller.getSnapshot().job.state, 'running');
  assert.equal(f.controller.getSnapshot().job.automatic_failures, 2);
  assert.equal(f.controller.getSnapshot().dialogOpen, false);
  assert.equal(f.calls.start, 0); assert.deepEqual(f.notices, []);
  f.api.getUpdateJob = async () => ({ ...job, state: 'succeeded', restart_required: true, automatic_failures: 0 });
  await f.advance(60000); assert.equal(f.notices.length, 1);
  f.controller.stop();
});

await test('reopening failed automatic jobs does not count them again or open the dialog', async () => {
  const job = { job_id: 'failed', state: 'failed', automatic: true, target_version: '2.0.0', automatic_failures: 3 };
  const f = fixture({ getLatestUpdateJob: async () => job });
  await f.controller.start(); await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(automaticUpdateFailed(available, f.controller.getSnapshot().job), true);
  assert.equal(f.controller.getSnapshot().dialogOpen, false); assert.deepEqual(f.notices, []);
  f.controller.stop();
});

await test('manual check joining a failed background check gets one error and no empty dialog', async () => {
  const response = deferred(); const f = fixture({ getUpdateStatus: () => response.promise });
  const startup = f.controller.start(); await flush();
  const manual = f.controller.check({ manual: true });
  response.resolve({ status: 'manifest_unavailable', error: 'offline' });
  await Promise.all([startup, manual]);
  assert.equal(f.notices.length, 1); assert.equal(f.notices[0].kind, 'err');
  assert.equal(f.controller.getSnapshot().dialogOpen, false); f.controller.stop();
});

await test('late checks after unmount cannot start installation or reschedule', async () => {
  const response = deferred(); const f = fixture({ getUpdateStatus: () => response.promise,
    getUpgradeConfig: async () => ({ auto_update: true }) });
  const work = f.controller.start(); await flush(); f.controller.stop();
  response.resolve(available); await work;
  assert.equal(f.calls.start, 0); assert.equal(f.timers.size, 0); assert.deepEqual(f.notices, []);
});

await test('double manual clicks start once and automatic conflicts retain silent task origin', async () => {
  const f = fixture(); await f.controller.start();
  const response = deferred(); let starts = 0;
  f.api.startUpdate = () => { starts += 1; return response.promise; };
  const first = f.controller.startUpdate(); const second = f.controller.startUpdate();
  await flush(); assert.equal(starts, 1);
  response.resolve({ job_id: 'one', state: 'running', automatic: true });
  await Promise.all([first, second]);
  f.api.getUpdateJob = async () => ({ job_id: 'one', state: 'failed', automatic: true, automatic_failures: 1 });
  await f.advance(350); assert.deepEqual(f.notices, []); f.controller.stop();
});

await test('disabled or store-managed automatic updates never start', async () => {
  const f = fixture({ getUpdateStatus: async () => ({ status: 'store_managed', update_available: false }),
    getUpgradeConfig: async () => ({ auto_update: true }) });
  await f.controller.start(); await f.advance(MIN_UPDATE_CHECK_DELAY_MS);
  assert.equal(f.calls.start, 0); assert.deepEqual(f.notices, []); f.controller.stop();
});

await test('manual cancellation is terminal and a late progress response cannot restore running', async () => {
  const f = fixture(); await f.controller.start();
  const progress = deferred(); f.api.getUpdateJob = () => progress.promise;
  await f.controller.startUpdate();
  f.controller.setJob({ job_id: 'job-1', state: 'cancelled', automatic: false });
  progress.resolve({ job_id: 'job-1', state: 'running' }); await flush();
  assert.equal(f.controller.getSnapshot().job.state, 'cancelled');
  assert.equal(f.notices.length, 1); assert.equal(f.notices[0].kind, 'info');
  assert.equal([...f.timers.values()].filter((t) => t.delay < MIN_UPDATE_CHECK_DELAY_MS).length, 0);
  f.controller.stop();
});

await test('configuration changes during a check recheck the saved preference after it settles', async () => {
  const config = deferred(); let reads = 0;
  const f = fixture({ getUpgradeConfig: () => ++reads === 1 ? config.promise : Promise.resolve({ auto_update: true }) });
  const startup = f.controller.start(); await flush();
  f.controller.refreshConfiguration(); config.resolve({ auto_update: false });
  await startup; await flush();
  assert.equal(f.calls.check, 2); assert.equal(f.calls.start, 1); f.controller.stop();
});

await test('changing daemon connection invalidates pending automatic work', async () => {
  let sameConnection = true; const config = deferred();
  const f = fixture({ getUpgradeConfig: () => config.promise }, { isCurrentConnection: () => sameConnection });
  const startup = f.controller.start(); await flush();
  sameConnection = false; config.resolve({ auto_update: true }); await startup;
  assert.equal(f.calls.start, 0); assert.equal(f.timers.size, 0); assert.deepEqual(f.notices, []);
  f.controller.stop();
});
