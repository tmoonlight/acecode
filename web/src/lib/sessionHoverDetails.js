// Sidebar session hover details: pure presentation, viewport placement, and
// a short-lived Git-info cache. React owns only event wiring and portal DOM.

export const SESSION_HOVER_CARD_VIEWPORT_MARGIN_PX = 8;
export const SESSION_HOVER_CARD_GAP_PX = 8;
export const SESSION_HOVER_GIT_CACHE_TTL_MS = 30_000;

function finiteNumber(value, fallback = 0) {
  return Number.isFinite(value) ? value : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

function clamp(value, min, max) {
  return Math.min(Math.max(min, value), Math.max(min, max));
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
