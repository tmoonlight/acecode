import assert from 'node:assert/strict';
import {
  beginSessionOpen, recordSessionHistoryBytes, reportSessionOpen,
  sessionOpenPayload, trackSessionHistoryRequest,
} from './sessionOpenDiagnostics.js';

const open = beginSessionOpen('synthetic', 100);
assert.equal(trackSessionHistoryRequest('/api/sessions/synthetic/messages?since=0'), open);
recordSessionHistoryBytes(open, 123);
assert.equal(trackSessionHistoryRequest('/api/sessions/synthetic/messages?since=45'), null);
assert.deepEqual(sessionOpenPayload(open, 350), {
  session_id: 'synthetic', elapsed_ms: 250, history_requests: 1, history_bytes: 123,
});
let attempts = 0;
await reportSessionOpen('synthetic', () => { attempts += 1; throw new Error('offline'); }, 350);
await reportSessionOpen('synthetic', () => { attempts += 1; }, 400);
assert.equal(attempts, 1, 'failed telemetry is dropped without retry');

const old = beginSessionOpen('race', 0);
const current = beginSessionOpen('race', 100);
recordSessionHistoryBytes(old, 999);
recordSessionHistoryBytes(current, 50);
assert.equal(sessionOpenPayload(current, 150).history_bytes, 50);
await reportSessionOpen('race', async () => {});
console.log('[pass] session-open diagnostics payload, failure and navigation race');
