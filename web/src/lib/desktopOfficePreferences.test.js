import assert from 'node:assert/strict';
import { createDesktopOfficePreferences, isOfficeWelcomeVisible, OFFICE_PREFERENCES_EVENT } from './desktopOfficePreferences.js';
import { shouldPrepareDesktopGuidedTour } from './desktopGuidedTour.js';

// Native claiming may finish after the ordinary first-run tour starts preparing.
// A queued invitation must let that tour finish before taking its turn.
{
  const tour = { mode: 'shell', authState: 'ok', startupNavigationSettled: true, hasActiveSession: false };
  const queued = { requested: true, blocked: false, tourPreparing: true, tourRunning: false };
  assert.equal(isOfficeWelcomeVisible(queued), false);
  assert.equal(shouldPrepareDesktopGuidedTour({ ...tour, blocked: isOfficeWelcomeVisible(queued) }), true,
    'a welcome claimed during preparation cannot deadlock the guided tour');
  assert.equal(isOfficeWelcomeVisible({ ...queued, tourPreparing: false, tourRunning: true }), false);
  const visible = isOfficeWelcomeVisible({ ...queued, tourPreparing: false });
  assert.equal(visible, true, 'welcome appears once the tour finishes or is dismissed');
  assert.equal(shouldPrepareDesktopGuidedTour({ ...tour, blocked: visible }), false,
    'a visible invitation protects its focus from the tour');
  assert.equal(isOfficeWelcomeVisible({ ...queued, tourPreparing: false, blocked: true }), false);
}

const flush = () => new Promise(resolve => setImmediate(resolve));
const initial = { ok: true, available: true, enabled: false, welcomePending: true };
function fixture(overrides = {}) {
  const win = new EventTarget();
  let state = { ...initial };
  Object.assign(win, {
    aceDesktop_getOfficePreferences: async () => JSON.stringify(state),
    aceDesktop_setOfficeEnabled: async enabled => (state = { ...state, enabled, welcomePending: false }),
    aceDesktop_claimOfficeWelcome: async () => {
      const show = state.welcomePending;
      state = { ...state, welcomePending: false };
      return { ...state, show };
    }, ...overrides,
  });
  const store = createDesktopOfficePreferences(win);
  const emit = detail => { const event = new Event(OFFICE_PREFERENCES_EVENT); event.detail = detail; win.dispatchEvent(event); };
  return { store, emit };
}

{
  const { store, emit } = fixture();
  store.start(); await flush();
  assert.equal(store.getSnapshot().enabled, false);
  assert.equal((await store.claimWelcome()).show, true);
  assert.equal((await store.claimWelcome()).show, false);
  await store.setEnabled(true);
  assert.equal(store.getSnapshot().enabled, true);
  emit({ ...initial, enabled: false, welcomePending: false });
  assert.equal(store.getSnapshot().enabled, false, 'native close synchronizes menu and switch');
  await store.setEnabled(true);
  assert.equal(store.getSnapshot().enabled, true, 'closed office can be reenabled');
  store.dispose();
}
{
  let resolve;
  const { store, emit } = fixture({ aceDesktop_setOfficeEnabled: () => new Promise(r => { resolve = r; }) });
  store.start(); await flush();
  const pending = store.setEnabled(true);
  emit({ ...initial, enabled: false, welcomePending: false });
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
console.log('[pass] office preferences opt-in, once-only welcome, native close/reopen, race and bridge failures');
