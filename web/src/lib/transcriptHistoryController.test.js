import assert from 'node:assert/strict';
import { createTranscriptHistoryController } from './transcriptHistoryController.js';

const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };
function fixture() {
  let rowTop = 50;
  const el = {
    scrollTop: 0, clientHeight: 600, scrollHeight: 3000,
    getBoundingClientRect: () => ({ top: 0, bottom: 600 }),
    querySelectorAll: () => [row], contains: (node) => node === row,
  };
  const row = { getBoundingClientRect: () => ({ top: rowTop - el.scrollTop, bottom: rowTop + 100 - el.scrollTop, height: 100 }) };
  const state = { loadState: 'loaded', hiddenCount: 0, historyBefore: 'b1', historyHasMore: true };
  const phases = [], requests = [];
  let localCount = 0, shown = 0;
  const controller = createTranscriptHistoryController({
    getViewport: () => el,
    getBoundary: () => ({ getBoundingClientRect: () => ({ height: 40 }) }),
    getSnapshot: () => state,
    onPhase: (phase) => phases.push(phase), onReview() {},
    afterPaint: async () => {},
    revealLocal: () => { localCount++; state.hiddenCount = 0; rowTop += 200; el.scrollHeight += 200; },
    showAll: () => { shown++; },
    loadPage: () => new Promise((resolve) => requests.push(resolve)),
  });
  return {
    el, row, state, controller, requests, phases,
    get localCount() { return localCount; }, get shown() { return shown; },
    prepend(height) { rowTop += height; el.scrollHeight += height; state.historyBefore += '-older'; },
    upward: () => controller.onWheel({ deltaY: -100, target: el }),
  };
}

{
  const f = fixture();
  f.controller.onScroll();
  await flush();
  assert.equal(f.requests.length, 0, 'programmatic or initial top is not a paging request');
  f.upward(); await flush();
  assert.equal(f.requests.length, 1);
  f.upward(); f.controller.onScroll(); await flush();
  assert.equal(f.requests.length, 1, 'repeated gestures during loading do not queue pages');
  // Tail growth must not be compensated or consume the reading anchor.
  f.el.scrollHeight += 1000;
  f.controller.preserveAnchor();
  assert.equal(f.el.scrollTop, 0);
  f.prepend(300);
  f.requests[0]({ status: 'loaded' }); await flush();
  assert.equal(f.el.scrollTop, 300);
  assert.equal(f.row.getBoundingClientRect().top, 50);
  f.controller.onScroll();
  // Delayed image layout above the same row must still preserve its position.
  f.prepend(80); f.controller.preserveAnchor();
  assert.equal(f.el.scrollTop, 380);
  assert.equal(f.row.getBoundingClientRect().top, 50);
  await flush();
  assert.equal(f.requests.length, 1, 'layout and completion cannot auto-drain history');
  f.controller.cancelAnchor();
  f.el.scrollTop = 0;
  f.upward(); await flush();
  assert.equal(f.requests.length, 2, 'another deliberate visit can request the next page');
  f.state.historyHasMore = false;
  f.requests[1]({ status: 'loaded' }); await flush();
  f.upward(); await flush();
  assert.equal(f.requests.length, 2, 'exhausted history does not request again');
  console.log('[pass] history intent, single flight, tail growth, late layout and exhaustion');
}
{
  const f = fixture();
  f.upward(); await flush();
  f.controller.onWheel({ deltaY: 20, target: f.el });
  f.el.scrollTop = 20;
  f.controller.onScroll();
  f.prepend(300); f.requests[0]({ status: 'loaded' }); await flush();
  assert.equal(f.el.scrollTop, 320, 'new user scroll supersedes the request-start position');
  f.controller.onScroll();
  f.controller.cancelAnchor();
  f.el.scrollTop = 0;
  f.upward(); await flush();
  f.controller.cancelAnchor(); // Explicit jump to tail while HTTP is pending.
  f.el.scrollTop = 1200;
  f.prepend(400); f.requests[1]({ status: 'loaded' }); await flush();
  assert.equal(f.el.scrollTop, 1200, 'explicit navigation is never undone');
  console.log('[pass] scrolling and explicit navigation win over an old reading anchor');
}
{
  const f = fixture();
  f.state.hiddenCount = 300;
  f.upward(); await flush();
  assert.equal(f.localCount, 1);
  assert.equal(f.requests.length, 0, 'local hidden history is revealed before remote paging');
  assert.equal(f.el.scrollTop, 200);
  assert.deepEqual(f.phases, ['loading', 'idle']);
  console.log('[pass] local history uses the same loading and anchor transaction');
}
for (const outcome of ['error', 'reset', 'loaded']) {
  const f = fixture();
  f.upward(); await flush();
  f.requests[0]({ status: outcome }); await flush();
  assert.equal(f.phases.at(-1), outcome === 'reset' ? 'reset' : 'error');
  f.upward(); f.controller.onScroll(); await flush();
  assert.equal(f.requests.length, 1, 'failure/reset/no progress requires explicit retry');
  const retry = f.controller.reveal(); await flush();
  f.state.historyHasMore = false;
  f.requests[1]({ status: 'loaded' }); await retry;
  assert.equal(f.phases.at(-1), 'idle');
}
console.log('[pass] errors, cursor reset and no progress stop automatic retry');
{
  const f = fixture();
  f.upward(); await flush();
  f.controller.dispose();
  f.prepend(500); f.requests[0]({ status: 'loaded' }); await flush();
  assert.equal(f.el.scrollTop, 0);
  assert.equal(f.shown, 0);
  assert.deepEqual(f.phases, ['loading'], 'disposed scope cannot publish or reveal another session');
  console.log('[pass] disposal ignores late page completions');
}
{
  const f = fixture();
  f.el.scrollTop = 500;
  f.controller.onPointerDown();
  f.el.scrollTop = 0;
  f.controller.onScroll({ pointerActive: true }); await flush();
  assert.equal(f.requests.length, 1);
  f.controller.dispose();
  f.requests[0]({ status: 'loaded' }); await flush();
  const keyboard = fixture();
  keyboard.controller.onKeyDown({ key: 'Home', target: { closest: () => ({}) } });
  await flush();
  assert.equal(keyboard.requests.length, 0, 'editing keys do not page the transcript');
  keyboard.controller.onKeyDown({ key: 'Home' }); await flush();
  assert.equal(keyboard.requests.length, 1);
  keyboard.controller.dispose(); keyboard.requests[0]({ status: 'loaded' }); await flush();
  console.log('[pass] scrollbar and keyboard intent with editable exclusion');
}
{
  const f = fixture();
  f.controller.onTouchStart({ touches: [{ clientY: 100 }] });
  f.controller.onTouchMove({ touches: [{ clientY: 150 }] }); await flush();
  assert.equal(f.requests.length, 1, 'finger movement towards older history triggers at top');
  f.controller.onTouchEnd();
  f.controller.dispose(); f.requests[0]({ status: 'loaded' }); await flush();
  const reset = fixture();
  reset.upward(); await flush();
  reset.state.historyHasMore = false;
  reset.requests[0]({ status: 'reset' }); await flush();
  await reset.controller.reveal();
  assert.equal(reset.phases.at(-1), 'idle', 'retry clears a reset notice even when the rewritten history is short');
  console.log('[pass] touch intent and exhausted reset recovery');
}
