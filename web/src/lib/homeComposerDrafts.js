export const NO_WORKSPACE_HOME_DRAFT_KEY = '__no_workspace__';

export function homeComposerDraftKey(workspaceHash = '') {
  const normalized = String(workspaceHash || '');
  return normalized || NO_WORKSPACE_HOME_DRAFT_KEY;
}

export function homeComposerDraftText(drafts, workspaceHash = '') {
  if (!drafts || typeof drafts !== 'object' || Array.isArray(drafts)) return '';
  const value = drafts[homeComposerDraftKey(workspaceHash)];
  return typeof value === 'string' ? value : '';
}

export function updateHomeComposerDrafts(drafts, workspaceHash = '', text = '') {
  const current = drafts && typeof drafts === 'object' && !Array.isArray(drafts)
    ? drafts
    : {};
  const key = homeComposerDraftKey(workspaceHash);
  const nextText = typeof text === 'string' ? text : String(text || '');

  if (!nextText) {
    if (!Object.prototype.hasOwnProperty.call(current, key)) return current;
    const next = { ...current };
    delete next[key];
    return next;
  }

  if (current[key] === nextText) return current;
  return { ...current, [key]: nextText };
}

export function clearHomeComposerDraftIfMatch(drafts, workspaceHash = '', expectedText = '') {
  const normalizedExpected = typeof expectedText === 'string'
    ? expectedText
    : String(expectedText || '');
  if (homeComposerDraftText(drafts, workspaceHash) !== normalizedExpected) return drafts;
  return updateHomeComposerDrafts(drafts, workspaceHash, '');
}
