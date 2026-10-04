// Owns one history-reading operation. ChatView remains the owner of input and
// resize events; this controller never installs a second scroll listener.
export function captureTranscriptReadingAnchor(container) {
  if (!container) return null;
  const bounds = container.getBoundingClientRect();
  const element = Array.from(container.querySelectorAll('[data-chat-row="true"]'))
    .find((row) => {
      const rect = row.getBoundingClientRect();
      return rect.height > 0 && rect.bottom > bounds.top && rect.top < bounds.bottom;
    });
  return element ? { element, top: element.getBoundingClientRect().top - bounds.top } : null;
}

export function createTranscriptHistoryController(options) {
  let active = true;
  let generation = 0;
  let pending = null;
  let phase = 'idle';
  let anchor = null;
  let armed = false;
  let scrollGesture = false;
  let previousTop = null;
  let programmedTop = null;
  let touchY = null;

  const publish = (next) => { phase = next; options.onPhase(next); };
  const cancelAnchor = () => { anchor = null; armed = false; scrollGesture = false; programmedTop = null; };
  const atTop = () => {
    const el = options.getViewport();
    if (!el || el.clientHeight <= 0) return false;
    const height = options.getBoundary()?.getBoundingClientRect().height || 0;
    return el.scrollTop <= Math.max(2, Math.min(height, el.clientHeight * 0.1));
  };
  const preserveAnchor = () => {
    const el = options.getViewport();
    if (!active || !anchor || !el) return false;
    if (!el.contains(anchor.element)) { anchor = null; return false; }
    const delta = anchor.element.getBoundingClientRect().top - el.getBoundingClientRect().top - anchor.top;
    if (Math.abs(delta) > 0.25) {
      el.scrollTop += delta;
      programmedTop = el.scrollTop;
    }
    previousTop = el.scrollTop;
    return true;
  };
  const run = ({ all = false, compensate = true } = {}) => {
    if (!active) return Promise.resolve();
    if (pending) return pending;
    const snapshot = options.getSnapshot();
    if (snapshot.loadState !== 'loaded') return Promise.resolve();
    if (!snapshot.hiddenCount && !snapshot.historyHasMore) {
      if (phase !== 'idle') publish('idle');
      return Promise.resolve();
    }
    const epoch = generation;
    const current = () => active && generation === epoch;
    armed = false;
    if (compensate) {
      options.onReview();
      anchor = captureTranscriptReadingAnchor(options.getViewport());
    } else cancelAnchor();
    publish('loading');
    const task = (async () => {
      // Let the inline state paint, including when the batch is already local.
      await options.afterPaint();
      if (!current()) return;
      let outcome = 'idle';
      try {
        if (!all && options.getSnapshot().hiddenCount > 0) {
          options.revealLocal();
        } else {
          while (current() && options.getSnapshot().historyHasMore) {
            const before = options.getSnapshot().historyBefore;
            const result = await options.loadPage();
            if (!current()) return;
            if (result.status === 'stale') { cancelAnchor(); outcome = 'reset'; break; }
            if (result.status === 'reset') { cancelAnchor(); outcome = 'reset'; break; }
            if (result.status === 'error') { outcome = 'error'; break; }
            const next = options.getSnapshot();
            if (next.historyHasMore && next.historyBefore === before) { outcome = 'error'; break; }
            if (!all) break;
          }
          if (current() && outcome === 'idle') options.showAll();
        }
        await options.afterPaint();
        if (!current()) return;
        preserveAnchor();
        publish(outcome);
      } catch {
        if (current()) publish('error');
      } finally {
        if (current()) {
          // A gesture received during loading cannot queue another page.
          armed = false;
          scrollGesture = false;
          pending = null;
        }
      }
    })();
    pending = task;
    return task;
  };
  const maybeLoad = () => {
    if (active && armed && !pending && phase === 'idle' && atTop()) {
      armed = false;
      void run();
    }
  };
  const gesture = (upward) => {
    if (!active) return;
    if (pending && anchor) anchor = captureTranscriptReadingAnchor(options.getViewport());
    else anchor = null;
    programmedTop = null;
    scrollGesture = true;
    armed = upward && !pending;
    if (upward) maybeLoad();
  };
  return {
    reveal: () => run(),
    async expand({ loadAll = false, compensateScroll = false } = {}) {
      if (pending) await pending;
      if (!active) return;
      if (loadAll) return run({ all: true, compensate: compensateScroll });
      cancelAnchor();
      options.showAll();
    },
    cancelAnchor,
    preserveAnchor,
    hasAnchor: () => active && !!anchor,
    onScroll({ pointerActive = false } = {}) {
      const el = options.getViewport();
      if (!active || !el) return;
      const top = el.scrollTop;
      if (programmedTop !== null && Math.abs(top - programmedTop) <= 2) {
        programmedTop = null;
      } else if (scrollGesture || pointerActive || touchY !== null) {
        if (pending && anchor) anchor = captureTranscriptReadingAnchor(el);
        else anchor = null;
        if (!pending && pointerActive && previousTop !== null && top < previousTop) armed = true;
        maybeLoad();
      }
      previousTop = el.scrollTop;
    },
    onWheel(event) {
      if (!event.deltaY) return;
      const el = options.getViewport();
      // An inner code block/tool scroller owns its gesture until it reaches its edge.
      for (let node = event.target; node && node !== el; node = node.parentElement) {
        if (node.scrollHeight > node.clientHeight && node.clientHeight > 0
          && /auto|scroll/.test(globalThis.getComputedStyle?.(node)?.overflowY || '')
          && (event.deltaY < 0 ? node.scrollTop > 0 : node.scrollTop + node.clientHeight < node.scrollHeight)) return;
      }
      gesture(event.deltaY < 0);
    },
    onPointerDown() {
      if (pending && anchor) anchor = captureTranscriptReadingAnchor(options.getViewport());
      else anchor = null;
      armed = false;
      previousTop = options.getViewport()?.scrollTop ?? null;
    },
    onKeyDown(event) {
      if (event.target?.closest?.('input, textarea, select, [contenteditable="true"]')) return;
      if (['ArrowUp', 'PageUp', 'Home'].includes(event.key) || (event.key === ' ' && event.shiftKey)) gesture(true);
      else if (['ArrowDown', 'PageDown', 'End', ' '].includes(event.key)) gesture(false);
    },
    onTouchStart(event) { touchY = event.touches?.[0]?.clientY ?? null; },
    onTouchMove(event) {
      const next = event.touches?.[0]?.clientY;
      if (touchY !== null && next != null && next !== touchY) gesture(next > touchY);
      touchY = next ?? null;
    },
    onTouchEnd() { touchY = null; },
    activate() { active = true; },
    dispose() {
      active = false;
      generation += 1;
      pending = null;
      cancelAnchor();
    },
  };
}
