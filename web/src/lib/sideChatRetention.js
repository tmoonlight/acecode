export const SIDE_CHAT_RETENTION_KEY = 'ace.sideChatRetentionMinutes';
export const DEFAULT_SIDE_CHAT_RETENTION_MINUTES = 30;

export function isSideChatRetentionMinutes(value) {
  return Number.isSafeInteger(value) && value > 0;
}

export function sideChatRetentionMilliseconds(minutes) {
  return (isSideChatRetentionMinutes(minutes) ? minutes : DEFAULT_SIDE_CHAT_RETENTION_MINUTES) * 60_000;
}
