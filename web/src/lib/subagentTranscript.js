import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

export function isSubagentTranscriptItemVisible(item) {
  if (item?.kind !== 'tool') return true;
  return String(item.tool?.tool || '').trim().toLowerCase() !== 'askuserquestion';
}

export function projectSubagentTranscriptItems(items, options = {}) {
  return projectCollapsedTranscriptItems(items, {
    ...options,
    filterNormalizedItem: isSubagentTranscriptItemVisible,
  });
}
