import { clsx } from './format.js';
import { completionSummaryMarkdown } from './taskCompleteSummary.js';

export const DEFAULT_TRANSCRIPT_CAPABILITIES = Object.freeze({
  forkMessages: true,
  showMessageFooters: true,
  navigateFiles: true,
  showSelectionAnnotations: true,
  pauseTailOnToolReview: true,
  openSubagentTranscripts: true,
});

export const READ_ONLY_TRANSCRIPT_CAPABILITIES = Object.freeze({
  forkMessages: false,
  showMessageFooters: false,
  navigateFiles: false,
  showSelectionAnnotations: false,
  pauseTailOnToolReview: true,
  openSubagentTranscripts: false,
});

export function normalizeTranscriptCapabilities(capabilities = {}) {
  return {
    ...DEFAULT_TRANSCRIPT_CAPABILITIES,
    ...(capabilities && typeof capabilities === 'object' ? capabilities : {}),
  };
}

export function completionSummaryText(item) {
  return completionSummaryMarkdown(item, '已完成');
}

export function transcriptMessageText(item) {
  if (item?.kind === 'completion_summary') return completionSummaryText(item);
  if (item?.kind !== 'msg') return '';
  if (item.role === 'user' && typeof item.metadata?.display_text === 'string' && item.metadata.display_text) {
    return item.metadata.display_text;
  }
  return String(item.content || '');
}

export function transcriptMessageContextAttrs(item, { canFork = true } = {}) {
  const isCompletionSummary = item?.kind === 'completion_summary';
  if (item?.kind !== 'msg' && !isCompletionSummary) return {};
  const messageId = item.messageId || '';
  return {
    'data-desktop-message-id': messageId || undefined,
    'data-desktop-message-role': isCompletionSummary ? 'assistant' : (item.role || undefined),
    'data-desktop-message-text': transcriptMessageText(item) || undefined,
    'data-desktop-message-can-fork': canFork && messageId ? 'true' : undefined,
  };
}

export function transcriptItemRole(item) {
  if (item?.kind === 'msg') return item.role || '';
  return item?.kind || '';
}

export function transcriptRenderKind(item) {
  switch (item?.kind) {
    case 'activity_summary':
    case 'completion_summary':
    case 'media_group':
    case 'subagent_group':
    case 'termination_notice':
    case 'tool':
      return item.kind;
    case 'msg':
    default:
      return 'message';
  }
}

export function transcriptRowClassName(item, { nested = false, extra = '' } = {}) {
  if (nested) return clsx('flex flex-col', extra);
  return clsx(
    'ace-chat-row flex flex-col',
    transcriptItemRole(item) === 'system' && 'ace-chat-row-assistant-gutter',
    extra,
  );
}

export function transcriptRowAttrs(item, {
  nested = false,
  continuation = false,
  canFork = true,
} = {}) {
  const attrs = {
    'data-chat-kind': item?.kind || '',
    'data-chat-role': transcriptItemRole(item),
    ...transcriptMessageContextAttrs(item, { canFork }),
  };
  if (nested) return attrs;
  return {
    'data-chat-row': 'true',
    'data-chat-item-id': item?.id == null ? undefined : String(item.id),
    ...attrs,
    'data-chat-user-message': item?.kind === 'msg' && item.role === 'user' ? 'true' : undefined,
    'data-chat-message-ordinal': item?.kind === 'msg' && item.messageOrdinal != null
      ? String(item.messageOrdinal)
      : undefined,
    'data-chat-assistant-continuation': continuation ? 'true' : undefined,
  };
}
