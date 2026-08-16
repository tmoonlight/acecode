import { isDesktopShell } from './desktopShellMode.js';

export const DESKTOP_STARTUP_PROGRESS_EVENT = 'acecode:desktop-startup-progress';
export const DESKTOP_STARTUP_PROGRESS_VERSION = 1;

const MAX_HISTORY = 32;
const MAX_FRONTEND_MS = 24 * 60 * 60 * 1000;
const FRONTEND_STAGES = new Set([
  'web_bootstrap',
  'first_contentful_paint',
  'daemon_connecting',
  'daemon_connected',
  'daemon_connection_failed',
  'ui_ready',
]);
const reportedStagesByWindow = new WeakMap();

function finiteNumber(value) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

export function normalizeDesktopStartupEvent(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const sequence = finiteNumber(value.sequence);
  const elapsedMs = finiteNumber(value.elapsed_ms);
  const stage = typeof value.stage === 'string' ? value.stage.trim() : '';
  const message = typeof value.message === 'string' ? value.message.trim() : '';
  const source = value.source === 'frontend' ? 'frontend' : 'native';
  if (!Number.isInteger(sequence) || sequence <= 0 || sequence > Number.MAX_SAFE_INTEGER) return null;
  if (!stage || stage.length > 64 || !/^[a-z0-9_]+$/.test(stage)) return null;
  if (!message || message.length > 160) return null;
  if (elapsedMs === null || elapsedMs < 0 || elapsedMs > MAX_FRONTEND_MS) return null;

  const event = {
    sequence,
    stage,
    message,
    source,
    elapsed_ms: Math.round(elapsedMs),
    terminal: value.terminal === true,
  };
  if (value.frontend_ms !== undefined && value.frontend_ms !== null) {
    const frontendMs = finiteNumber(value.frontend_ms);
    if (frontendMs === null || frontendMs < 0 || frontendMs > MAX_FRONTEND_MS) return null;
    event.frontend_ms = frontendMs;
  }
  return event;
}

export function normalizeDesktopStartupSnapshot(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  if (Number(value.version) !== DESKTOP_STARTUP_PROGRESS_VERSION) return null;
  const history = Array.isArray(value.history)
    ? value.history.map(normalizeDesktopStartupEvent).filter(Boolean).slice(-MAX_HISTORY)
    : [];
  const current = normalizeDesktopStartupEvent(value.current)
    || history[history.length - 1]
    || null;
  if (!current) return null;
  return {
    version: DESKTOP_STARTUP_PROGRESS_VERSION,
    current,
    history,
  };
}

export function initialDesktopStartupProgress(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!isDesktopShell(win)) return null;
  return normalizeDesktopStartupSnapshot(win.__ACECODE_DESKTOP_STARTUP__);
}

export function subscribeDesktopStartupProgress(
  listener,
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!win || typeof listener !== 'function' || !isDesktopShell(win)) return () => {};
  const handleProgress = (event) => {
    const snapshot = normalizeDesktopStartupSnapshot(event?.detail);
    if (!snapshot) return;
    win.__ACECODE_DESKTOP_STARTUP__ = snapshot;
    listener(snapshot);
  };
  win.addEventListener?.(DESKTOP_STARTUP_PROGRESS_EVENT, handleProgress);
  return () => win.removeEventListener?.(DESKTOP_STARTUP_PROGRESS_EVENT, handleProgress);
}

export function reportDesktopStartupMilestone(
  stage,
  win = typeof window !== 'undefined' ? window : undefined,
  performanceApi = win?.performance,
) {
  if (!win || !isDesktopShell(win) || !FRONTEND_STAGES.has(stage)) return false;
  if (typeof win.aceDesktop_reportStartupMilestone !== 'function') return false;
  let reported = reportedStagesByWindow.get(win);
  if (!reported) {
    reported = new Set();
    reportedStagesByWindow.set(win, reported);
  }
  if (reported.has(stage)) return false;

  const measured = finiteNumber(performanceApi?.now?.());
  const performanceMs = measured !== null && measured >= 0 && measured <= MAX_FRONTEND_MS
    ? measured
    : undefined;
  reported.add(stage);
  try {
    const result = win.aceDesktop_reportStartupMilestone({
      stage,
      ...(performanceMs === undefined ? {} : { performance_ms: performanceMs }),
    });
    Promise.resolve(result).catch(() => reported.delete(stage));
    return true;
  } catch {
    reported.delete(stage);
    return false;
  }
}

export function installDesktopStartupPaintReporter(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  if (!win || !isDesktopShell(win)) return () => {};
  const performanceApi = win.performance;
  const reportEntry = (entry) => {
    if (!entry || entry.name !== 'first-contentful-paint') return false;
    return reportDesktopStartupMilestone(
      'first_contentful_paint',
      win,
      { now: () => finiteNumber(entry.startTime) ?? performanceApi?.now?.() },
    );
  };

  const buffered = performanceApi?.getEntriesByName?.('first-contentful-paint', 'paint') || [];
  if (buffered.some(reportEntry)) return () => {};

  const Observer = win.PerformanceObserver;
  if (typeof Observer !== 'function') return () => {};
  let observer;
  try {
    observer = new Observer((list) => {
      const entries = list?.getEntries?.() || [];
      if (entries.some(reportEntry)) observer?.disconnect?.();
    });
    try {
      observer.observe({ type: 'paint', buffered: true });
    } catch {
      observer.observe({ entryTypes: ['paint'] });
    }
  } catch {
    return () => {};
  }
  return () => observer?.disconnect?.();
}
