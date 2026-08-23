export const SIDEBAR_SESSION_LOAD_CONCURRENCY = 3;
export const SIDEBAR_SESSION_PENDING_SETTLE_MS = 100;
export const SIDEBAR_SESSION_LOAD_CACHE_LIMIT = 12;

function normalizedKey(value) {
  return String(value || '').trim();
}

export function sidebarSessionLoadKey({
  sessionId = '',
  workspaceHash = '',
  noWorkspace = false,
} = {}) {
  const sid = normalizedKey(sessionId);
  if (!sid) return '';
  const scope = noWorkspace ? 'no-workspace' : (normalizedKey(workspaceHash) || 'local');
  return `${scope}:${sid}`;
}

function deferredJob(key, load, queuedAt) {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return {
    key,
    load,
    queuedAt,
    promise,
    resolve,
    reject,
  };
}

export function createSidebarSessionLoadPool({
  maxConcurrent = SIDEBAR_SESSION_LOAD_CONCURRENCY,
  pendingSettleMs = SIDEBAR_SESSION_PENDING_SETTLE_MS,
  cacheLimit = SIDEBAR_SESSION_LOAD_CACHE_LIMIT,
  now = () => Date.now(),
  setTimer = (fn, delay) => setTimeout(fn, delay),
  clearTimer = (timer) => clearTimeout(timer),
  onChange = () => {},
} = {}) {
  const capacity = Math.max(1, Math.trunc(Number(maxConcurrent) || 1));
  const settleMs = Math.max(0, Math.trunc(Number(pendingSettleMs) || 0));
  const maxCache = Math.max(0, Math.trunc(Number(cacheLimit) || 0));
  const running = new Map();
  const cache = new Map();
  let pendingLatest = null;
  let pendingTimer = null;
  let disposed = false;

  const snapshot = () => ({
    runningKeys: Array.from(running.keys()),
    pendingKey: pendingLatest?.key || '',
    cacheKeys: Array.from(cache.keys()),
    maxConcurrent: capacity,
  });

  const emit = () => {
    if (!disposed) onChange(snapshot());
  };

  const clearPendingTimer = () => {
    if (pendingTimer == null) return;
    clearTimer(pendingTimer);
    pendingTimer = null;
  };

  const settlePending = (status = 'superseded') => {
    clearPendingTimer();
    const previous = pendingLatest;
    pendingLatest = null;
    if (previous) previous.resolve({ status, key: previous.key });
    return previous;
  };

  const remember = (key, value) => {
    if (maxCache <= 0 || disposed) return;
    cache.delete(key);
    cache.set(key, value);
    while (cache.size > maxCache) {
      const oldest = cache.keys().next().value;
      cache.delete(oldest);
    }
  };

  let drainPending = () => {};

  const start = (job) => {
    if (!job || disposed) {
      job?.resolve({ status: 'superseded', key: job.key });
      return;
    }
    clearPendingTimer();
    if (pendingLatest === job) pendingLatest = null;
    running.set(job.key, job);
    emit();

    let result;
    try {
      result = job.load();
    } catch (error) {
      result = Promise.reject(error);
    }
    Promise.resolve(result)
      .then((value) => {
        remember(job.key, value);
        job.resolve({ status: 'loaded', key: job.key, value });
      })
      .catch((error) => {
        cache.delete(job.key);
        job.reject(error);
      })
      .finally(() => {
        if (running.get(job.key) === job) running.delete(job.key);
        emit();
        drainPending();
      });
  };

  drainPending = () => {
    if (disposed || !pendingLatest || running.size >= capacity) return;
    clearPendingTimer();
    const elapsed = Math.max(0, Number(now()) - pendingLatest.queuedAt);
    const remaining = Math.max(0, settleMs - elapsed);
    if (remaining > 0) {
      pendingTimer = setTimer(() => {
        pendingTimer = null;
        drainPending();
      }, remaining);
      return;
    }
    start(pendingLatest);
  };

  const clearPendingForDifferentTarget = (key) => {
    if (!pendingLatest || pendingLatest.key === key) return false;
    settlePending('superseded');
    emit();
    return true;
  };

  return {
    request(keyValue, load) {
      const key = normalizedKey(keyValue);
      if (disposed || !key || typeof load !== 'function') {
        return Promise.resolve({ status: 'superseded', key });
      }

      if (cache.has(key)) {
        clearPendingForDifferentTarget(key);
        const value = cache.get(key);
        cache.delete(key);
        cache.set(key, value);
        return Promise.resolve({ status: 'cached', key, value });
      }

      const runningJob = running.get(key);
      if (runningJob) {
        clearPendingForDifferentTarget(key);
        return runningJob.promise;
      }

      if (pendingLatest?.key === key) return pendingLatest.promise;

      const job = deferredJob(key, load, Number(now()));
      const replacingPending = !!pendingLatest;
      if (pendingLatest) settlePending('superseded');

      if (running.size < capacity && !replacingPending) {
        start(job);
      } else {
        pendingLatest = job;
        emit();
        drainPending();
      }
      return job.promise;
    },

    cancelPending() {
      const cancelled = settlePending('superseded');
      if (cancelled) emit();
      return !!cancelled;
    },

    invalidate(keyValue) {
      return cache.delete(normalizedKey(keyValue));
    },

    statusFor(keyValue) {
      const key = normalizedKey(keyValue);
      if (!key) return 'idle';
      if (pendingLatest?.key === key) return 'queued';
      if (running.has(key)) return 'loading';
      if (cache.has(key)) return 'cached';
      return 'idle';
    },

    snapshot,

    dispose() {
      if (disposed) return;
      settlePending('superseded');
      disposed = true;
      cache.clear();
    },
  };
}
