const MAX_HISTORY = 80;
const SEP = '\u001f';
const TRANSFER_VERSION = 1;
const TRANSFER_HASH_KEY = 'ace_nav';
const MAX_TRANSFER_PAYLOAD_LENGTH = 64 * 1024;
const MAX_IDENTITY_LENGTH = 2048;
const MAX_LABEL_LENGTH = 512;

function refValue(ref, ...keys) {
  for (const key of keys) {
    const value = ref && ref[key];
    if (value != null && value !== '') return String(value);
  }
  return '';
}

function boundedRefValue(ref, keys, maxLength = MAX_IDENTITY_LENGTH) {
  const value = refValue(ref, ...keys);
  return value && value.length <= maxLength ? value : '';
}

function transferableRef(ref) {
  if (!ref || typeof ref !== 'object' || Array.isArray(ref)) return null;
  const out = {};
  const workspaceHash = boundedRefValue(ref, ['workspaceHash', 'workspace_hash']);
  const sessionId = boundedRefValue(ref, ['sessionId', 'session_id', 'id']);
  const contextId = boundedRefValue(ref, ['contextId', 'context_id']);
  const cwd = boundedRefValue(ref, ['cwd']);
  const expertId = boundedRefValue(ref, ['expertId', 'expert_id']);
  const externalSurface = boundedRefValue(ref, ['externalSurface', 'external_surface'], 64);
  const displayTitle = boundedRefValue(ref, ['displayTitle', 'display_title'], MAX_LABEL_LENGTH);
  const title = boundedRefValue(ref, ['title'], MAX_LABEL_LENGTH);
  const workspaceName = boundedRefValue(ref, ['workspaceName', 'workspace_name'], MAX_LABEL_LENGTH);

  if (workspaceHash) out.workspaceHash = workspaceHash;
  if (sessionId) out.sessionId = sessionId;
  if (contextId) out.contextId = contextId;
  if (cwd) out.cwd = cwd;
  if (expertId) out.expertId = expertId;
  if (externalSurface) out.externalSurface = externalSurface;
  if (displayTitle) out.displayTitle = displayTitle;
  if (title) out.title = title;
  if (workspaceName) out.workspaceName = workspaceName;
  for (const [key, aliases] of [
    ['home', ['home']],
    ['homeWorkspaceExplicit', ['homeWorkspaceExplicit', 'home_workspace_explicit']],
    ['loop', ['loop']],
    ['expertComponents', ['expertComponents', 'expert_components']],
    ['noWorkspace', ['noWorkspace', 'no_workspace']],
    ['readOnly', ['readOnly', 'read_only']],
  ]) {
    if (aliases.some((alias) => ref[alias] === true)) out[key] = true;
  }

  const match = ref.searchMatch || ref.search_match;
  const ordinal = Number(match?.messageOrdinal ?? match?.message_ordinal);
  if (Number.isInteger(ordinal) && ordinal >= 0) {
    out.searchMatch = {
      kind: 'user_message',
      messageOrdinal: ordinal,
      message_ordinal: ordinal,
    };
  }

  if (!out.home && !out.loop && !out.sessionId && !out.expertComponents && !out.expertId) {
    return null;
  }
  return out;
}

function transferableHistory(history) {
  const normalized = normalizeHistory(history);
  return {
    back: normalized.back.map(transferableRef).filter(Boolean),
    forward: normalized.forward.map(transferableRef).filter(Boolean),
  };
}

function transferPayload(history) {
  const transferable = transferableHistory(history);
  const payload = {
    v: TRANSFER_VERSION,
    b: transferable.back,
    f: transferable.forward,
  };
  let serialized = JSON.stringify(payload);
  while (serialized.length > MAX_TRANSFER_PAYLOAD_LENGTH
      && (payload.b.length > 0 || payload.f.length > 0)) {
    if (payload.b.length >= payload.f.length && payload.b.length > 0) payload.b.shift();
    else payload.f.pop();
    serialized = JSON.stringify(payload);
  }
  return serialized.length <= MAX_TRANSFER_PAYLOAD_LENGTH ? serialized : '';
}

export function navigationKey(ref) {
  if (!ref || typeof ref !== 'object') return '';
  const type = ref.loop ? 'loop' : ref.home ? 'home' : 'session';
  return [
    type,
    refValue(ref, 'workspaceHash', 'workspace_hash'),
    refValue(ref, 'sessionId', 'session_id', 'id'),
    refValue(ref, 'contextId', 'context_id'),
    refValue(ref, 'cwd'),
  ].join(SEP);
}

export function sameNavigationRef(a, b) {
  return navigationKey(a) === navigationKey(b);
}

export function pushNavigation(history, currentRef, nextRef) {
  const currentKey = navigationKey(currentRef);
  const nextKey = navigationKey(nextRef);
  if (!currentKey || currentKey === nextKey) {
    return normalizeHistory(history);
  }
  const back = [...normalizeHistory(history).back, currentRef].slice(-MAX_HISTORY);
  return { back, forward: [] };
}

export function goBack(history, currentRef) {
  const normalized = normalizeHistory(history);
  if (normalized.back.length === 0) {
    return { history: normalized, activeRef: currentRef };
  }
  const activeRef = normalized.back[normalized.back.length - 1];
  const back = normalized.back.slice(0, -1);
  const forward = navigationKey(currentRef)
    ? [currentRef, ...normalized.forward].slice(0, MAX_HISTORY)
    : normalized.forward;
  return { history: { back, forward }, activeRef };
}

export function goForward(history, currentRef) {
  const normalized = normalizeHistory(history);
  if (normalized.forward.length === 0) {
    return { history: normalized, activeRef: currentRef };
  }
  const activeRef = normalized.forward[0];
  const forward = normalized.forward.slice(1);
  const back = navigationKey(currentRef)
    ? [...normalized.back, currentRef].slice(-MAX_HISTORY)
    : normalized.back;
  return { history: { back, forward }, activeRef };
}

export function normalizeHistory(history) {
  return {
    back: Array.isArray(history?.back) ? history.back.filter(Boolean).slice(-MAX_HISTORY) : [],
    forward: Array.isArray(history?.forward) ? history.forward.filter(Boolean).slice(0, MAX_HISTORY) : [],
  };
}

export function serializeNavigationHistory(history) {
  return transferPayload(history);
}

export function deserializeNavigationHistory(value) {
  if (typeof value !== 'string' || !value || value.length > MAX_TRANSFER_PAYLOAD_LENGTH) {
    return null;
  }
  try {
    const payload = JSON.parse(value);
    if (payload?.v !== TRANSFER_VERSION
        || !Array.isArray(payload.b)
        || !Array.isArray(payload.f)) {
      return null;
    }
    return transferableHistory({ back: payload.b, forward: payload.f });
  } catch {
    return null;
  }
}

export function navigationHistoryFromHash(hash = '') {
  const raw = String(hash || '').replace(/^#/, '');
  if (!raw) return null;
  const params = new URLSearchParams(raw);
  if (!params.has(TRANSFER_HASH_KEY)) return null;
  return deserializeNavigationHistory(params.get(TRANSFER_HASH_KEY) || '');
}

export function navigationHistoryHash(history) {
  const payload = serializeNavigationHistory(history);
  if (!payload) return '';
  const params = new URLSearchParams();
  params.set(TRANSFER_HASH_KEY, payload);
  return params.toString();
}

export function stripNavigationHistoryHash(hash = '') {
  const raw = String(hash || '').replace(/^#/, '');
  if (!raw) return '';
  const params = new URLSearchParams(raw);
  if (!params.has(TRANSFER_HASH_KEY)) return raw;
  params.delete(TRANSFER_HASH_KEY);
  return params.toString();
}
