import assert from 'node:assert/strict';
import { createDesktopOfficePreferences, OFFICE_PREFERENCES_EVENT } from './desktopOfficePreferences.js';

const flush = () => new Promise(resolve => setImmediate(resolve));
const initial = { ok: true, available: true, enabled: false };
function fixture(overrides = {}) {
  const win = new EventTarget();
  let state = { ...initial };
  Object.assign(win, {
    aceDesktop_getOfficePreferences: async () => JSON.stringify(state),
    aceDesktop_setOfficeEnabled: async enabled => (state = { ...state, enabled }),
    aceDesktop_setPetKind: async petKind => (state = { ...state, petKind }),
    ...overrides,
  });
  const store = createDesktopOfficePreferences(win);
  const emit = detail => { const event = new Event(OFFICE_PREFERENCES_EVENT); event.detail = detail; win.dispatchEvent(event); };
  return { store, emit };
}

{
  const { store, emit } = fixture();
  store.start(); await flush();
  assert.equal(store.getSnapshot().enabled, false);
  await store.setEnabled(true);
  assert.equal(store.getSnapshot().enabled, true);
  emit({ ...initial, enabled: false });
  assert.equal(store.getSnapshot().enabled, false, 'native close synchronizes menu and switch');
  await store.setEnabled(true);
  assert.equal(store.getSnapshot().enabled, true, 'closed office can be reenabled');
  store.dispose();
}
// Legacy hosts may still report pending announcements. They must not enter UI state.
{
  let claims = 0;
  const legacy = { ...initial, welcomePending: true, petsPromoPending: true };
  const { store, emit } = fixture({
    aceDesktop_getOfficePreferences: async () => JSON.stringify(legacy),
    aceDesktop_claimOfficeWelcome: async () => { claims++; return legacy; },
    aceDesktop_claimPetsPromo: async () => { claims++; return legacy; },
  });
  store.start(); await flush();
  assert.equal('welcomePending' in store.getSnapshot(), false);
  assert.equal('petsPromoPending' in store.getSnapshot(), false);
  emit({ ...legacy, enabled: true, petKind: 'whale' });
  assert.equal(store.getSnapshot().enabled, true);
  assert.equal(store.getSnapshot().petKind, 'whale');
  assert.equal('welcomePending' in store.getSnapshot(), false);
  assert.equal('petsPromoPending' in store.getSnapshot(), false);
  assert.equal(claims, 0);
  store.dispose();
}
{
  let resolve;
  const { store, emit } = fixture({ aceDesktop_setOfficeEnabled: () => new Promise(r => { resolve = r; }) });
  store.start(); await flush();
  const pending = store.setEnabled(true);
  emit({ ...initial, enabled: false });
  resolve({ ...initial, enabled: true }); await pending;
  assert.equal(store.getSnapshot().enabled, false, 'late enable response does not undo a native close');
  assert.equal(store.getSnapshot().busy, false);
  store.dispose();
}
{
  const { store } = fixture({ aceDesktop_setOfficeEnabled: async () => { throw new Error('bridge failed'); } });
  store.start(); await flush();
  assert.equal((await store.setEnabled(true)).ok, false);
  assert.equal(store.getSnapshot().enabled, false);
  assert.ok(store.getSnapshot().error);
  assert.equal(store.getSnapshot().busy, false);
  store.dispose();
  const unavailable = createDesktopOfficePreferences(undefined);
  unavailable.start();
  assert.equal(unavailable.getSnapshot().available, false);
  assert.equal(unavailable.getSnapshot().ready, true);
  unavailable.dispose();
}
console.log('[pass] office preferences opt-in, legacy announcement flags ignored, native close/reopen, race and bridge failures');

{
  const { store, emit } = fixture();
  store.start(); await flush();
  assert.equal(store.getSnapshot().petKind, 'office', 'old hosts default to the office');
  for (const petKind of ['clippy', 'rover', 'whale', 'office']) {
    assert.equal((await store.setPetKind(petKind)).ok, true);
    assert.equal(store.getSnapshot().petKind, petKind);
    assert.equal(store.getSnapshot().enabled, false, 'choosing a pet does not enable it');
  }
  assert.equal((await store.setPetKind('unknown')).ok, false);
  emit({ ...initial, petKind: {} });
  assert.equal(store.getSnapshot().petKind, 'office');
  store.dispose();
}
{
  const { store } = fixture({ aceDesktop_setPetKind: async () => ({ ...initial, ok: false, error: 'save_failed' }) });
  store.start(); await flush();
  const result = await store.setPetKind('whale');
  assert.equal(result.ok, false);
  assert.equal(result.error, 'save_failed');
  assert.equal(store.getSnapshot().petKind, 'office');
  assert.ok(store.getSnapshot().error);
  assert.equal(store.getSnapshot().busy, false);
  store.dispose();
}
console.log('[pass] desktop pet selection, invalid kinds, old hosts and failed persistence');
