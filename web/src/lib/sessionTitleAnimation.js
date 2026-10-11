export function sessionTitleAnimationKey(session = {}) {
  session = session || {};
  const id = session.sessionId || session.session_id || session.id || '';
  if (!id) return '';
  const hash = session.workspaceHash || session.workspace_hash || '';
  const scope = session.noWorkspace || session.no_workspace
    ? 'no-workspace' : (hash === '__local__' ? '' : hash);
  // A resumed header may carry a daemon port that the same sidebar row omits.
  return JSON.stringify([scope, id]);
}

const segmenter = typeof Intl?.Segmenter === 'function'
  ? new Intl.Segmenter(undefined, { granularity: 'grapheme' }) : null;

export function titleGraphemes(title) {
  // Older WebViews show the complete title rather than split composed characters.
  return segmenter ? Array.from(segmenter.segment(title), (part) => part.segment) : [title];
}

// One clock and immutable snapshot per visible session, shared by all title surfaces.
export function createSessionTitleAnimationStore({
  now, schedule, cancel, shouldAnimate = () => true, reducedMotion = () => false,
}) {
  const records = new Map();
  let pending = null;
  const ensure = (key) => {
    if (!records.has(key)) records.set(key, { listeners: new Set(), frame: null });
    return records.get(key);
  };
  const publish = (record, text, typing) => {
    if (record.frame?.title === record.title && record.frame?.text === text && record.frame?.typing === typing) return;
    record.frame = { title: record.title, text, typing };
    for (const listener of record.listeners) listener();
  };
  const isRunning = () => [...records.values()].some((record) => record.frame?.typing);
  const tick = () => {
    pending = null;
    const time = now();
    for (const record of records.values()) {
      if (!record.frame?.typing) continue;
      // Retain brief title-change feedback even when Windows disables UI animations.
      const duration = reducedMotion() ? Math.min(320, record.duration) : record.duration;
      const progress = shouldAnimate() ? Math.min(1, (time - record.startedAt) / duration) : 1;
      const count = 1 + Math.floor(progress * (record.characters.length - 1));
      publish(record, record.characters.slice(0, count).join(''), progress < 1);
    }
    if (isRunning()) pending = schedule(tick);
  };
  const stopIdleClock = () => {
    if (!isRunning() && pending !== null) {
      cancel(pending);
      pending = null;
    }
  };
  return {
    subscribe(key, listener) {
      if (!key) return () => {};
      const record = ensure(key);
      record.listeners.add(listener);
      return () => {
        record.listeners.delete(listener);
        if (!record.listeners.size) records.delete(key);
        stopIdleClock();
      };
    },
    getSnapshot(key) { return records.get(key)?.frame || null; },
    start(key, title) {
      if (!key) return;
      const record = ensure(key);
      if (record.title === title) return;
      record.title = title;
      record.characters = titleGraphemes(title);
      record.startedAt = now();
      record.duration = Math.min(650, Math.max(180, record.characters.length * 28));
      const typing = shouldAnimate() && record.characters.length > 1;
      publish(record, typing ? record.characters[0] : title, typing);
      if (typing && pending === null) pending = schedule(tick);
      stopIdleClock();
    },
    finish(key) {
      const record = records.get(key);
      if (record?.frame?.typing) publish(record, record.title, false);
      stopIdleClock();
    },
  };
}
