const OPTIMISTIC_METADATA_KEY = 'optimistic_new_session_input';

function normalizedText(value) {
  return String(value ?? '').trim();
}

function displayedUserText(item) {
  const displayText = item?.metadata?.display_text;
  if (typeof displayText === 'string' && displayText.trim()) return displayText;
  return item?.content || '';
}

function isMatchingCanonicalUserMessage(item, pending) {
  if (item?.kind !== 'msg' || item.role !== 'user') return false;
  const metadata = item.metadata && typeof item.metadata === 'object'
    ? item.metadata
    : {};
  if (
    metadata[OPTIMISTIC_METADATA_KEY] === true
    || metadata.synthetic_user_prompt === true
    || metadata.hidden_goal_context === true
  ) {
    return false;
  }
  return normalizedText(displayedUserText(item)) === pending.normalizedText;
}

export function createPendingNewSessionFirstUserMessage({
  sessionId,
  text,
  timestampMs = Date.now(),
} = {}) {
  const normalizedSessionId = String(sessionId || '').trim();
  const content = String(text ?? '');
  const comparableText = normalizedText(content);
  if (!normalizedSessionId || !comparableText) return null;

  const parsedTimestamp = Number(timestampMs);
  const ts = Number.isFinite(parsedTimestamp) ? parsedTimestamp : Date.now();
  return {
    sessionId: normalizedSessionId,
    normalizedText: comparableText,
    item: {
      kind: 'msg',
      id: `optimistic-new-session-user:${normalizedSessionId}`,
      messageId: '',
      role: 'user',
      content,
      contentParts: [],
      metadata: {
        display_text: content,
        [OPTIMISTIC_METADATA_KEY]: true,
      },
      ts,
      streaming: false,
    },
  };
}

export function withPendingNewSessionFirstUserMessage(
  items,
  pending,
  currentSessionId,
) {
  const source = Array.isArray(items) ? items : [];
  if (
    !pending
    || !pending.item
    || !pending.normalizedText
    || pending.sessionId !== String(currentSessionId || '').trim()
  ) {
    return source;
  }
  if (source.some((item) => isMatchingCanonicalUserMessage(item, pending))) {
    return source;
  }
  return [pending.item, ...source];
}

export const __test__ = {
  displayedUserText,
  isMatchingCanonicalUserMessage,
  normalizedText,
};
