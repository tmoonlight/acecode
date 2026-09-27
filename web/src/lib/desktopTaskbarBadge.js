import { colorContrast } from './colorContrast.js';
import { normalizeThemeBackgroundColor } from './desktopWindowBackground.js';
import { normalizeStatusPayload, sessionAttentionState } from './sessionStatus.js';

export function desktopTaskbarBadgeAvailable(win = globalThis.window) {
  return typeof win?.aceDesktop_setTaskbarBadge === 'function';
}

export function taskbarBadgeColors(style) {
  const read = (name) => normalizeThemeBackgroundColor(style.getPropertyValue(name));
  const background = read('--ace-accent');
  const outline = read('--ace-bg');
  const text = read('--ace-fg');
  if (!background || !outline || !text) return null;
  let foreground = colorContrast(background, text) >= colorContrast(background, outline) ? text : outline;
  if (colorContrast(background, foreground) < 4.5) {
    foreground = colorContrast(background, '#ffffff') >= colorContrast(background, '#000000') ? '#ffffff' : '#000000';
  }
  return { background, foreground, outline };
}

function mainTask(session) {
  return session && !session.archived && !session.parent_session_id && !session.parentSessionId;
}

// Track authoritative root-task membership separately from paginated UI lists.
export function createTaskbarBadgeController({
  getWindow = () => globalThis.window,
  getDocument = () => globalThis.document,
  schedule = (callback) => setTimeout(callback, 50),
  cancel = (timer) => clearTimeout(timer),
} = {}) {
  const scopes = new Map();
  let allowedScopes = null;
  let timer = null;
  let lastSent = '';

  function count() {
    const unread = new Set();
    for (const sessions of scopes.values()) {
      for (const [id, session] of sessions) {
        if (sessionAttentionState(session) === 'unread') unread.add(id);
      }
    }
    return unread.size;
  }

  function flush() {
    if (timer !== null) cancel(timer);
    timer = null;
    const win = getWindow();
    if (!desktopTaskbarBadgeAvailable(win)) return false;
    const unread = count();
    let payload = { count: unread };
    if (unread) {
      let colors;
      try {
        colors = taskbarBadgeColors(win.getComputedStyle(getDocument().documentElement));
      } catch { return false; }
      if (!colors) return false;
      payload = { ...payload, ...colors };
    }
    const key = JSON.stringify(payload);
    if (lastSent === key) return false;
    const failed = () => { if (lastSent === key) lastSent = ''; };
    try {
      lastSent = key;
      Promise.resolve(win.aceDesktop_setTaskbarBadge(payload)).then((result) => {
        try {
          const response = typeof result === 'string' ? JSON.parse(result) : result;
          if (response?.ok === false) failed();
        } catch { failed(); }
      }, failed);
      return true;
    } catch {
      failed();
      return false;
    }
  }

  function refresh() {
    if (!desktopTaskbarBadgeAvailable(getWindow()) || timer !== null) return;
    timer = schedule(flush);
  }

  function replaceScope(scope, sessions) {
    if (!desktopTaskbarBadgeAvailable(getWindow())) return;
    const key = String(scope || '');
    if (allowedScopes && !allowedScopes.has(key)) return;
    const next = new Map();
    const previous = scopes.get(key);
    for (const session of sessions || []) {
      if (!mainTask(session)) continue;
      const status = normalizeStatusPayload(session);
      if (!status) continue;
      const old = previous?.get(status.session_id);
      // A list request can finish after a newer read acknowledgement or result.
      const newer = old && (old.cursor > status.cursor ||
        (old.read_cursor > status.read_cursor && old.read_cursor >= status.cursor));
      next.set(status.session_id, newer ? old : status);
    }
    scopes.set(key, next);
    refresh();
  }

  function updateStatus(payload) {
    const status = normalizeStatusPayload(payload);
    if (!status) return;
    // Unknown IDs may be children; only root-task snapshots establish membership.
    for (const sessions of scopes.values()) {
      const previous = sessions.get(status.session_id);
      if (!previous) continue;
      if (status.timestamp_ms && previous.timestamp_ms > status.timestamp_ms) continue;
      if (!mainTask(payload)) sessions.delete(status.session_id);
      else sessions.set(status.session_id, { ...previous, ...status });
    }
    refresh();
  }

  return {
    count,
    flush,
    refresh,
    replaceScope,
    updateStatus,
    retainWorkspaces(hashes) {
      if (!desktopTaskbarBadgeAvailable(getWindow())) return;
      allowedScopes = new Set(['', ...hashes]);
      for (const scope of scopes.keys()) if (!allowedScopes.has(scope)) scopes.delete(scope);
      refresh();
    },
    handleMessage(message = {}) {
      if (!desktopTaskbarBadgeAvailable(getWindow())) return;
      const payload = message.payload || {};
      if (message.type === 'session_status_snapshot') {
        replaceScope(payload.workspace_hash || message.workspace_hash || '', payload.sessions || []);
      } else if (message.type === 'session_status' || message.type === 'mark_session_read_ack'
        || message.type === 'mark_session_unread_ack') {
        updateStatus({ ...payload, session_id: payload.session_id || message.session_id });
      }
    },
    removeSession(id) {
      for (const sessions of scopes.values()) sessions.delete(id);
      refresh();
    },
    dispose() {
      if (timer !== null) cancel(timer);
      timer = null;
      scopes.clear();
      allowedScopes = null;
      flush();
    },
  };
}

export const desktopTaskbarBadge = createTaskbarBadgeController();
