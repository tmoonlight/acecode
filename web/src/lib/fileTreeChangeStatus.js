const STATUS_LABELS = {
  M: '已修改',
  U: '新增',
  D: '已删除',
};

const STATUS_PRIORITY = {
  U: 1,
  M: 2,
  D: 3,
};

function finiteNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) ? n : 0;
}

export function normalizeTreePath(path) {
  return String(path || '')
    .replace(/\\/g, '/')
    .replace(/^\/\/\?\//, '')
    .replace(/^\.\/+/, '')
    .replace(/\/+/g, '/')
    .replace(/^\/+/, '')
    .replace(/\/+$/, '');
}

export function normalizeWorkspaceRelativePath(path, cwd = '') {
  const normalized = normalizeTreePath(path);
  const root = normalizeTreePath(cwd);
  if (!normalized || !root) return normalized;

  const normalizedLower = normalized.toLowerCase();
  const rootLower = root.toLowerCase();
  if (normalizedLower === rootLower) return '';
  if (normalizedLower.startsWith(`${rootLower}/`)) {
    return normalized.slice(root.length + 1);
  }
  return normalized;
}

function normalizeStatus(status) {
  const s = String(status || '').toUpperCase();
  return Object.prototype.hasOwnProperty.call(STATUS_LABELS, s) ? s : '';
}

function strongerStatus(current, candidate) {
  const a = normalizeStatus(current);
  const b = normalizeStatus(candidate);
  if (!b) return a;
  if (!a || STATUS_PRIORITY[b] > STATUS_PRIORITY[a]) return b;
  return a;
}

function isDescendantPath(child, parent) {
  const c = normalizeTreePath(child);
  const p = normalizeTreePath(parent);
  if (!c) return false;
  if (!p) return c.includes('/');
  return c.startsWith(`${p}/`);
}

export function fileChangeStatusLabel(status) {
  return STATUS_LABELS[normalizeStatus(status)] || '';
}

export function fileChangeStatusTitle(status, isDir = false) {
  const label = fileChangeStatusLabel(status);
  if (!label) return '';
  return isDir ? `目录内有变更: ${label}` : label;
}

export function reviewStatusForGroup(group) {
  const explicit = normalizeStatus(group?.status || group?.review_status);
  if (explicit) return explicit;

  const additions = finiteNumber(group?.totalAdditions);
  const deletions = finiteNumber(group?.totalDeletions);
  const hunks = Array.isArray(group?.hunks) ? group.hunks : [];
  const hasHunks = hunks.length > 0;
  const oldSideEmpty = hasHunks && hunks.every((hunk) => finiteNumber(hunk?.old_count) === 0);
  const newSideEmpty = hasHunks && hunks.every((hunk) => finiteNumber(hunk?.new_count) === 0);

  if (additions > 0 && deletions === 0 && oldSideEmpty) return 'U';
  if (deletions > 0 && additions === 0 && newSideEmpty) return 'D';
  if (additions > 0 || deletions > 0 || hasHunks) return 'M';
  return '';
}

export function buildReviewStatusMap(groups, cwd = '') {
  const statuses = new Map();
  if (!Array.isArray(groups)) return statuses;
  for (const group of groups) {
    const path = normalizeWorkspaceRelativePath(group?.file, cwd);
    const status = reviewStatusForGroup(group);
    if (!path || !status) continue;
    statuses.set(path, strongerStatus(statuses.get(path), status));
  }
  return statuses;
}

export function statusForTreeEntry(entry, statusByPath) {
  if (!entry || !(statusByPath instanceof Map)) return '';
  const path = normalizeTreePath(entry.path);
  let status = normalizeStatus(statusByPath.get(path));
  if (entry.kind !== 'dir') return status;

  for (const [changedPath, changedStatus] of statusByPath.entries()) {
    if (isDescendantPath(changedPath, path)) {
      status = strongerStatus(status, changedStatus);
    }
  }
  return status;
}

export function entriesWithReviewStatuses(entries, statusByPath) {
  const listedEntries = Array.isArray(entries) ? entries : [];
  return listedEntries.map((entry) => {
    const reviewStatus = statusForTreeEntry(entry, statusByPath);
    return reviewStatus ? { ...entry, review_status: reviewStatus } : { ...entry };
  });
}
