// Sidebar session hover details: pure presentation, viewport placement, and
// a short-lived Git-info cache. React owns only event wiring and portal DOM.

export const SESSION_HOVER_CARD_VIEWPORT_MARGIN_PX = 8;
export const SESSION_HOVER_CARD_GAP_PX = 8;
export const SESSION_HOVER_GIT_CACHE_TTL_MS = 30_000;
export const SESSION_HOVER_LIFECYCLE_ACTIONS = Object.freeze({
  POINTER_ENTER: 'pointer-enter',
  POINTER_LEAVE: 'pointer-leave',
  KEYBOARD_ENTER: 'keyboard-enter',
  KEYBOARD_LEAVE: 'keyboard-leave',
  CLEAR_KEYBOARD: 'clear-keyboard',
  CLEAR_OWNER: 'clear-owner',
  CLEAR_ALL: 'clear-all',
});

const EMPTY_SESSION_HOVER_LIFECYCLE_STATE = Object.freeze({
  pointerOwner: '',
  keyboardOwner: '',
});

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

function clamp(value, min, max) {
  return Math.min(Math.max(min, value), Math.max(min, max));
}

function hoverOwner(value) {
  return typeof value === 'string' ? value : '';
}

export function createSessionHoverLifecycleState() {
  return { ...EMPTY_SESSION_HOVER_LIFECYCLE_STATE };
}

export function activeSessionHoverOwner(state = EMPTY_SESSION_HOVER_LIFECYCLE_STATE) {
  return hoverOwner(state?.pointerOwner) || hoverOwner(state?.keyboardOwner);
}

export function reduceSessionHoverLifecycle(
  state = EMPTY_SESSION_HOVER_LIFECYCLE_STATE,
  action = {},
) {
  const current = state && typeof state === 'object'
    ? state
    : EMPTY_SESSION_HOVER_LIFECYCLE_STATE;
  const pointerOwner = hoverOwner(current.pointerOwner);
  const keyboardOwner = hoverOwner(current.keyboardOwner);
  const owner = hoverOwner(action.owner);

  switch (action.type) {
    case SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_ENTER:
      if (!owner || pointerOwner === owner) return current;
      return { pointerOwner: owner, keyboardOwner };
    case SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_LEAVE:
      if (!owner || pointerOwner !== owner) return current;
      return { pointerOwner: '', keyboardOwner };
    case SESSION_HOVER_LIFECYCLE_ACTIONS.KEYBOARD_ENTER:
      if (!owner || keyboardOwner === owner) return current;
      return { pointerOwner, keyboardOwner: owner };
    case SESSION_HOVER_LIFECYCLE_ACTIONS.KEYBOARD_LEAVE:
      if (!owner || keyboardOwner !== owner) return current;
      return { pointerOwner, keyboardOwner: '' };
    case SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_KEYBOARD:
      if (!keyboardOwner) return current;
      return { pointerOwner, keyboardOwner: '' };
    case SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_OWNER: {
      if (!owner || (pointerOwner !== owner && keyboardOwner !== owner)) return current;
      return {
        pointerOwner: pointerOwner === owner ? '' : pointerOwner,
        keyboardOwner: keyboardOwner === owner ? '' : keyboardOwner,
      };
    }
    case SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_ALL:
      if (!pointerOwner && !keyboardOwner) return current;
      return createSessionHoverLifecycleState();
    default:
      return current;
  }
}

export function sessionHoverFocusIsVisible(target, { pointerInitiated = false } = {}) {
  if (pointerInitiated) return false;
  if (typeof target?.matches !== 'function') return true;
  try {
    return target.matches(':focus-visible');
  } catch {
    // Older embedded WebViews may not parse :focus-visible. Pointer origin is
    // still known synchronously, so non-pointer focus remains accessible.
    return true;
  }
}

export function sessionHoverDetails(session, gitInfo = null) {
  if (!session || typeof session !== 'object') return null;
  if (session.noWorkspace || session.no_workspace) return null;

  const cwd = typeof session.cwd === 'string' ? session.cwd : '';
  if (!cwd.trim()) return null;

  const isGitRepository = gitInfo?.is_repo === true;
  return {
    cwd,
    branch: isGitRepository ? String(gitInfo.branch || 'HEAD') : '',
    isGitRepository,
  };
}

export function computeSessionHoverCardPosition({
  anchorRect = {},
  cardWidth,
  cardHeight,
  viewportWidth,
  viewportHeight,
  margin = SESSION_HOVER_CARD_VIEWPORT_MARGIN_PX,
  gap = SESSION_HOVER_CARD_GAP_PX,
} = {}) {
  const safeViewportWidth = nonNegativeNumber(viewportWidth);
  const safeViewportHeight = nonNegativeNumber(viewportHeight);
  const safeMargin = Math.min(
    nonNegativeNumber(margin),
    safeViewportWidth / 2,
    safeViewportHeight / 2,
  );
  const availableWidth = Math.max(0, safeViewportWidth - (safeMargin * 2));
  const availableHeight = Math.max(0, safeViewportHeight - (safeMargin * 2));
  const width = Math.min(nonNegativeNumber(cardWidth), availableWidth);
  const height = Math.min(nonNegativeNumber(cardHeight), availableHeight);
  const safeGap = nonNegativeNumber(gap);

  const anchorLeft = finiteNumber(anchorRect.left);
  const anchorTop = finiteNumber(anchorRect.top);
  const anchorWidth = nonNegativeNumber(anchorRect.width);
  const anchorHeight = nonNegativeNumber(anchorRect.height);
  const anchorRight = finiteNumber(anchorRect.right, anchorLeft + anchorWidth);

  const roomRight = Math.max(0, safeViewportWidth - safeMargin - anchorRight);
  const roomLeft = Math.max(0, anchorLeft - safeMargin);
  const placement = roomRight >= width + safeGap || roomRight >= roomLeft
    ? 'right'
    : 'left';
  const preferredLeft = placement === 'right'
    ? anchorRight + safeGap
    : anchorLeft - safeGap - width;
  const preferredTop = anchorTop + (anchorHeight / 2) - (height / 2);

  return {
    placement,
    left: Math.round(clamp(
      preferredLeft,
      safeMargin,
      safeViewportWidth - safeMargin - width,
    )),
    top: Math.round(clamp(
      preferredTop,
      safeMargin,
      safeViewportHeight - safeMargin - height,
    )),
    maxHeight: Math.floor(availableHeight),
  };
}

export function createSessionHoverGitInfoCache(
  loadGitInfo,
  {
    now = () => Date.now(),
    ttlMs = SESSION_HOVER_GIT_CACHE_TTL_MS,
  } = {},
) {
  if (typeof loadGitInfo !== 'function') {
    throw new TypeError('loadGitInfo must be a function');
  }

  const entries = new Map();
  const safeTtlMs = nonNegativeNumber(ttlMs);

  const get = (cwd) => {
    const key = typeof cwd === 'string' ? cwd : '';
    if (!key.trim()) return Promise.resolve(null);

    const timestamp = finiteNumber(now());
    const existing = entries.get(key);
    if (existing?.promise) return existing.promise;
    if (existing && existing.expiresAt >= timestamp) {
      return Promise.resolve(existing.value);
    }

    let request;
    request = Promise.resolve()
      .then(() => loadGitInfo(key))
      .then(
        (value) => {
          if (entries.get(key)?.promise === request) {
            entries.set(key, {
              value,
              expiresAt: finiteNumber(now()) + safeTtlMs,
            });
          }
          return value;
        },
        (error) => {
          if (entries.get(key)?.promise === request) entries.delete(key);
          throw error;
        },
      );
    entries.set(key, { promise: request });
    return request;
  };

  return {
    get,
    invalidate(cwd = '') {
      const key = typeof cwd === 'string' ? cwd : '';
      if (key) entries.delete(key);
      else entries.clear();
    },
  };
}
