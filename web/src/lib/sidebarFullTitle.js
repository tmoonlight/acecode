import { isGeneratedErrorTitle, titleFromMessages } from './sessionTitle.js';

export const STORAGE_SUMMARY_TRUNCATION_SUFFIX = '...';

function text(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function sessionId(session = {}) {
  return text(session.id || session.session_id || session.sessionId || '');
}

function workspaceHash(session = {}) {
  return text(session.workspace_hash || session.workspaceHash || '');
}

export function sidebarTitleHydrationState(session = {}, displayedTitle = '') {
  const display = text(displayedTitle);
  const explicit = text(session.title);
  const summary = text(session.summary);
  const hasUsableExplicitTitle = Boolean(explicit) && !isGeneratedErrorTitle(session);
  const usesSummary = Boolean(summary)
    && display === summary
    && (!hasUsableExplicitTitle || display !== explicit);
  const needsFullTitle = usesSummary
    && summary.endsWith(STORAGE_SUMMARY_TRUNCATION_SUFFIX);

  if (!needsFullTitle) {
    return { displayTitle: display, needsFullTitle: false };
  }

  return {
    displayTitle: summary
      .slice(0, -STORAGE_SUMMARY_TRUNCATION_SUFFIX.length)
      .trimEnd(),
    needsFullTitle: true,
  };
}

export function sidebarFullTitleRequestKey(session = {}) {
  const id = sessionId(session);
  if (!id) return '';
  return [
    workspaceHash(session),
    id,
    Number(session.turn_count ?? session.turnCount ?? 0) || 0,
    Number(session.message_count ?? session.messageCount ?? 0) || 0,
    text(session.summary),
  ].join('\u0000');
}

export function createSidebarFullTitleLoader() {
  const resolved = new Map();
  const pending = new Map();

  return async function loadSidebarFullTitle(apiClient, session = {}) {
    const key = sidebarFullTitleRequestKey(session);
    const id = sessionId(session);
    if (!key || !id || typeof apiClient?.getMessages !== 'function') return '';
    if (resolved.has(key)) return resolved.get(key);
    if (pending.has(key)) return pending.get(key);

    const request = Promise.resolve(
      apiClient.getMessages(id, 0, workspaceHash(session)),
    ).then((data) => {
      const messages = Array.isArray(data?.messages) ? data.messages : [];
      const fullTitle = titleFromMessages(messages);
      if (fullTitle) resolved.set(key, fullTitle);
      return fullTitle;
    }).finally(() => {
      pending.delete(key);
    });

    pending.set(key, request);
    return request;
  };
}

export const loadSidebarFullTitle = createSidebarFullTitleLoader();
