export const CHANGE_DOCK_DISMISSALS_STORAGE_KEY = 'acecode.changeDockDismissals.v1';

const MAX_DISMISSED_DOCK_ENTRIES = 200;

function isRecord(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export function validateDockDismissals(value) {
  if (!isRecord(value)) return false;
  return Object.entries(value).every(([key, signature]) => (
    typeof key === 'string'
    && key.length > 0
    && typeof signature === 'string'
    && signature.length > 0
  ));
}

function normalizedDockDismissals(value) {
  if (!isRecord(value)) return {};
  const out = {};
  for (const [key, signature] of Object.entries(value)) {
    if (typeof key === 'string' && key && typeof signature === 'string' && signature) {
      out[key] = signature;
    }
  }
  return out;
}

export function dockDismissalKey(sessionRef, sessionId = '') {
  const sid = String(sessionId || sessionRef?.sessionId || '');
  if (!sid) return '';
  const workspaceHash = String(sessionRef?.workspaceHash || '');
  return workspaceHash ? `${workspaceHash}:${sid}` : sid;
}

export function dismissedDockSignatureFor(dismissals, key) {
  if (!key || !isRecord(dismissals)) return '';
  const value = dismissals[key];
  return typeof value === 'string' ? value : '';
}

export function dismissChangeDockSignature(dismissals, key, signature) {
  if (!key || !signature) return normalizedDockDismissals(dismissals);
  const next = normalizedDockDismissals(dismissals);
  delete next[key];
  next[key] = signature;

  const keys = Object.keys(next);
  if (keys.length <= MAX_DISMISSED_DOCK_ENTRIES) return next;

  const trimmed = { ...next };
  for (const staleKey of keys.slice(0, keys.length - MAX_DISMISSED_DOCK_ENTRIES)) {
    delete trimmed[staleKey];
  }
  return trimmed;
}