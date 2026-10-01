
import assert from 'node:assert/strict';
import React from 'react';
import { useSessionTranscript } from './sessionTranscript.js';
import { connection } from './connection.js';

// Exercise the real hook with a deterministic React dispatcher. Effects and
// cleanup follow dependency changes; network completion is controlled by tests.
function hookHarness() {
  const slots = [];
  let index = 0;
  let effects = [];
  const same = (a, b) => a && b && a.length === b.length && a.every((x, i) => Object.is(x, b[i]));
  const dispatcher = {
    useRef(value) { const slot = index++; return slots[slot] ??= { current: value }; },
    useMemo(factory, deps) {
      const slot = index++;
      if (!slots[slot] || !same(slots[slot].deps, deps)) slots[slot] = { deps, value: factory() };
      return slots[slot].value;
    },
    useCallback(callback, deps) { return dispatcher.useMemo(() => callback, deps); },
    useEffect(effect, deps) {
      const slot = index++;
      if (!slots[slot] || !same(slots[slot].deps, deps)) {
        effects.push(() => {
          slots[slot]?.cleanup?.();
          slots[slot] = { deps, cleanup: effect() };
        });
      }
    },
    useSyncExternalStore(_subscribe, getSnapshot) { index++; return getSnapshot(); },
  };
  const internals = React.__SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED;
  return {
    render(ref) {
      index = 0;
      effects = [];
      const previous = internals.ReactCurrentDispatcher.current;
      internals.ReactCurrentDispatcher.current = dispatcher;
      let result;
      try { result = useSessionTranscript(ref, { live: true }); }
      finally { internals.ReactCurrentDispatcher.current = previous; }
      effects.forEach((effect) => effect());
      return result;
    },
    dispose() { slots.forEach((slot) => slot?.cleanup?.()); },
  };
}

const previous = {
  fetch: globalThis.fetch, location: globalThis.location, window: globalThis.window,
  sessionStorage: globalThis.sessionStorage, raf: globalThis.requestAnimationFrame,
  cancelRaf: globalThis.cancelAnimationFrame,
};
const methods = ['reconfigure', 'retainSession', 'releaseSession'];
const originals = methods.map((key) => connection[key]);
try {
  globalThis.location = { protocol: 'http:' };
  globalThis.window = { location: { href: 'http://127.0.0.1/' } };
  globalThis.sessionStorage = { getItem: () => '' };
  globalThis.requestAnimationFrame = () => 1;
  globalThis.cancelAnimationFrame = () => {};
  methods.forEach((key) => { connection[key] = () => {}; });
  for (const completeBeforeResume of [true, false]) {
    const requests = [];
    let complete;
    globalThis.fetch = (url) => {
      requests.push(url);
      return new Promise((resolve) => { complete = () => resolve({
        ok: true, status: 200,
        headers: { get: (key) => key === 'Content-Type' ? 'application/json' : '100' },
        json: async () => ({ messages: [{ role: 'user', content: 'Synthetic history', uuid: 'u1' }], busy: false }),
      }); });
    };
    const hook = hookHarness();
    const ref = { sessionId: 'loading-regression', port: 31000, token: 'synthetic', resumePending: true };
    hook.render(ref);
    assert.equal(requests.length, 1);
    if (completeBeforeResume) { complete(); await new Promise(resolve => setTimeout(resolve, 0)); }
    const liveView = hook.render({ ...ref, resumePending: false, active: true });
    liveView.applyEvent({ type: 'message', seq: 1, payload: {
      role: 'system', content: 'resume audit', id: 'resume-audit',
    } });
    assert.equal(requests.length, 1, 'resume/live transition must not fetch persisted history again');
    if (!completeBeforeResume) { complete(); await new Promise(resolve => setTimeout(resolve, 0)); }
    const loaded = hook.render({ ...ref, resumePending: false, active: true });
    assert.equal(loaded.loadState, 'loaded', 'in-flight disk load survives runtime activation');
    assert.equal(loaded.isLive, true);
    assert.ok(loaded.items.some(item => item.content === 'resume audit'), 'recovery event survives an older in-flight snapshot');
    assert.ok(loaded.items.some(item => item.content === 'Synthetic history' || item.text === 'Synthetic history'));
    hook.dispose();
  }
} finally {
  Object.assign(globalThis, { fetch: previous.fetch, location: previous.location, window: previous.window,
    sessionStorage: previous.sessionStorage, requestAnimationFrame: previous.raf,
    cancelAnimationFrame: previous.cancelRaf });
  methods.forEach((key, i) => { connection[key] = originals[i]; });
}
console.log('[pass] one history request across resume, before and during disk loading');
