import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Run the production hook with deterministic React effects and deferred HTTP.
const moduleUrl = new URL('./sessionTranscript.js', import.meta.url);
const source = readFileSync(moduleUrl, 'utf8');
const dependencies = {};
const imports = /^import \{([^}]+)\} from '([^']+)';$/gm;
for (const [, names, specifier] of source.matchAll(imports)) {
  if (specifier === 'react') continue;
  const module = await import(new URL(specifier, moduleUrl));
  for (const name of names.split(',').map((value) => value.trim())) dependencies[name] = module[name];
}
const body = source.replace(imports, '').replace(/^export /gm, '');

function harness() {
  const slots = [], requests = [], retained = [];
  let cursor = 0, effects = [];
  let poll = () => {};
  const changed = (previous, next) => !previous || next.some((value, i) => value !== previous[i]);
  const hooks = {
    useRef(initial) {
      const index = cursor++;
      return slots[index] ||= { current: initial };
    },
    useMemo(factory, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) slots[index] = { deps, value: factory() };
      return slots[index].value;
    },
    useCallback(callback, deps) { return hooks.useMemo(() => callback, deps); },
    useSyncExternalStore(_subscribe, snapshot) { cursor++; return snapshot(); },
    useEffect(callback, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) {
        const previous = slots[index];
        slots[index] = { deps };
        effects.push(() => { previous?.cleanup?.(); slots[index].cleanup = callback(); });
      }
    },
  };
  const hook = vm.runInNewContext(`${body}\nuseSessionTranscript;`, {
    ...dependencies, ...hooks, console, Map, Set,
    window: { setInterval: (fn) => { poll = fn; return 1; }, clearInterval() {} },
    requestAnimationFrame: () => 1, cancelAnimationFrame() {},
    createApi: () => ({
      getMessages: (sid, since) => new Promise((resolve, reject) => requests.push({ sid, since, resolve, reject })),
    }),
    connection: {
      reconfigure() {}, addEventListener() {}, removeEventListener() {},
      retainSession: (sid) => retained.push(sid), releaseSession() {},
    },
  });
  return {
    requests, retained, poll: () => poll(),
    render(ref, options = {}) {
      cursor = 0; effects = [];
      const result = hook(ref, { live: true, ...options });
      effects.forEach((effect) => effect());
      return result;
    },
    unmount() { slots.forEach((slot) => slot?.cleanup?.()); },
  };
}
const flush = async () => { for (let i = 0; i < 6; i++) await Promise.resolve(); };
const history = { messages: [{ role: 'user', content: 'saved history', ts: 1 }], events: [] };

const h = harness();
const pending = { sessionId: 's1', resumePending: true };
h.render(pending);
assert.equal(h.retained.length, 0);
h.requests[0].resolve(history);
await flush();
let view = h.render(pending);
assert.equal(view.loadState, 'loaded');
assert.equal(view.items[0].content, 'saved history');

view = h.render({ sessionId: 's1', active: true, resumePending: false });
assert.equal(view.getState().loadState, 'loaded');
assert.equal(view.getState().items[0].content, 'saved history');
assert.deepEqual(h.retained, ['s1']);
assert.equal(h.requests.length, 1, 'promotion does not reload persisted history');
console.log('[pass] live promotion preserves disk history and subscribes without another history request');

view = h.render({ sessionId: 's2', resumePending: true });
assert.equal(view.getState().loadState, 'loading');
assert.equal(view.getState().items.length, 0);
view = h.render({ sessionId: 's3', resumePending: true });
h.requests[1].resolve(history);
await flush();
assert.equal(view.getState().items.length, 0, 'late s2 response must not populate s3');
console.log('[pass] navigation clears the previous transcript and ignores stale recovery responses');
h.requests[2].resolve(history);
await flush();
view = h.render({ sessionId: 's3', resumeFailed: true });
assert.equal(view.items[0].content, 'saved history');
assert.equal(view.isLive, false);
assert.deepEqual(h.retained, ['s1']);
console.log('[pass] failed resume leaves disk history visible without retaining a live session');
view = h.render({ sessionId: 's3', resumePending: true, port: 12345 });
assert.equal(view.getState().items.length, 0, 'changing daemon identity must reset history');
h.unmount();

const pageHarness = harness();
const disk = { sessionId: 'paged', resumePending: true };
const options = { live: false, refreshIntervalMs: 1500 };
pageHarness.render(disk, options);
assert.equal(pageHarness.requests[0].since.limit, 200);
pageHarness.requests[0].resolve({
  messages: [{ id: 'u2', role: 'user', content: 'tail', message_position: '200' }],
  has_more: true, before: 'before-200', after: 'after-200', turn_truncated: false,
});
await flush();
let paged = pageHarness.render(disk, options);
assert.equal(paged.historyHasMore, true);
assert.equal(paged.items[0].messagePosition, '200');
const originalId = paged.items[0].id;
const older = paged.loadEarlier();
assert.equal(pageHarness.requests[1].since.before, 'before-200');
pageHarness.requests[1].resolve({
  messages: [{ id: 'u1', role: 'user', content: 'older', message_position: '0' }],
  has_more: false, after: 'after-0',
});
await older;
paged = pageHarness.render(disk, options);
assert.equal(paged.items[1].id, originalId, 'prepend keeps the window/DOM anchor identity');
assert.equal(paged.historyAfter, 'after-200', 'older pages never move the polling cursor backwards');
assert.equal(paged.historyHasMore, false);
const sameItems = paged.items;
pageHarness.poll();
assert.equal(pageHarness.requests[2].since.after, 'after-200');
pageHarness.requests[2].resolve({ messages: [], after: 'after-200' });
await flush();
assert.equal(paged.getState().items, sameItems, 'empty incremental refresh preserves all items');
pageHarness.poll();
pageHarness.requests[3].reject({ status: 409 });
await flush();
assert.equal(pageHarness.requests[4].since.limit, 200, 'stale poll reloads a bounded tail');
pageHarness.requests[4].resolve({
  messages: [{ role: 'user', content: 'rewritten', message_position: '0' }],
  has_more: true, before: 'new-before', after: 'new-after',
});
await flush();
paged = pageHarness.render(disk, options);
assert.equal(paged.items[0].content, 'rewritten');
const staleOlder = paged.loadEarlier();
pageHarness.requests[5].reject({ status: 409 });
await flush();
assert.equal(pageHarness.requests[6].since.limit, 200);
pageHarness.requests[6].resolve({ messages: [{ role: 'user', content: 'final' }], has_more: false });
await staleOlder;
assert.equal(paged.getState().items[0].content, 'final');
pageHarness.unmount();
console.log('[pass] paged history preserves anchors, polls incrementally and reloads stale cursors');
