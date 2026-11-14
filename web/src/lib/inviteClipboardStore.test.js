import assert from 'node:assert/strict';
import { createInviteClipboardStore } from './inviteClipboardStore.js';

const writes = [];
let complete;
const store = createInviteClipboardStore({
  get: async () => ({ ok: true, enabled: true }),
  set: (value) => { writes.push(value); return new Promise((resolve) => { complete = resolve; }); },
});
await store.load();
const saving = store.set(false);
void store.set(true);
complete({ ok: true });
await Promise.resolve();
assert.deepEqual(writes, [false, true]);
complete({ ok: true });
await saving;
assert.equal(store.getSnapshot().enabled, true);
assert.equal(store.getSnapshot().saving, false);
let failed = true;
const retry = createInviteClipboardStore({ get: async () => ({ ok: true, enabled: true }), set: async () => ({ ok: !failed }) });
await retry.load();
await retry.set(false);
assert.equal(retry.getSnapshot().enabled, false);
assert.ok(retry.getSnapshot().error);
failed = false;
await retry.retry();
assert.equal(retry.getSnapshot().error, '');
console.log('inviteClipboardStore tests passed');
