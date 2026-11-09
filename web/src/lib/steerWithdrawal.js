// 撤回插话(对话末尾淡化气泡上的叉):模型还没读到的那条消息退回输入框,而不是丢掉。
//
// 拼接规则照 Claude Code 把排队消息拉回输入框的做法:输入框是空的就直接换成退回的消息;
// 已经有内容时,退回的消息放在前面,换行后接原来的内容。附件、粘贴块、@ 引用都在
// composer_content 里随文字一起回来;上下文芯片(contexts)单独并回芯片列表。
import { queuedInputRequestPayload } from './chatInputQueue.js';
import {
  COMPOSER_CONTENT_VERSION,
  composerContentAttachments,
  composerContentFromText,
  composerContentText,
  normalizeComposerContent,
} from './composerContent.js';

function emptyComposerContent() {
  return normalizeComposerContent({ version: COMPOSER_CONTENT_VERSION, parts: [] });
}

function isEmptyComposerContent(content) {
  return !content.parts.some((part) => part.type !== 'text') && composerContentText(content) === '';
}

export function prependComposerContent(front, current) {
  const head = normalizeComposerContent(front) || emptyComposerContent();
  const tail = normalizeComposerContent(current) || emptyComposerContent();
  if (isEmptyComposerContent(head)) return tail;
  if (isEmptyComposerContent(tail)) return head;
  return normalizeComposerContent({
    version: COMPOSER_CONTENT_VERSION,
    parts: [...head.parts, { type: 'text', text: '\n' }, ...tail.parts],
  });
}

function contextKey(context) {
  return String(context?.local_id || context?.id || '');
}

// 退回的上下文芯片排在现有芯片前面;同一个芯片(local_id / id 相同)只留一份。
export function mergeWithdrawnContexts(withdrawn, current) {
  const merged = [];
  const seen = new Set();
  for (const context of [...withdrawn, ...current]) {
    if (!context || typeof context !== 'object') continue;
    const key = contextKey(context);
    if (key && seen.has(key)) continue;
    if (key) seen.add(key);
    merged.push(context);
  }
  return merged;
}

// queuedItem:撤回成功的那条插话(排队状态里的条目)。返回要写回输入框的完整状态:
//   content    拼好的 composer_content(交给输入框的 setComposerContent)
//   resources  退回消息里的附件资源(先并进附件列表,输入框才认得这些附件块)
//   contexts   合并后的上下文芯片
// 拿不到这条消息的内容时返回 null(什么都不改)。
export function withdrawnSteerComposerState(queuedItem, {
  currentContent = null,
  currentText = '',
  currentContexts = [],
  sessionId = '',
} = {}) {
  const payload = queuedInputRequestPayload(queuedItem);
  if (!payload) return null;
  const withdrawnContent = normalizeComposerContent(payload.composer_content)
    || composerContentFromText(payload.text || '', payload.attachments || []);
  const current = normalizeComposerContent(currentContent) || composerContentFromText(currentText || '');
  const contexts = (Array.isArray(payload.contexts) ? payload.contexts : []).map((context, index) => (
    contextKey(context) ? context : { ...context, local_id: `withdrawn-${payload.client_message_id}-${index}` }
  ));
  return {
    content: prependComposerContent(withdrawnContent, current),
    resources: composerContentAttachments(withdrawnContent, payload.attachments || [], { sessionId }),
    contexts: mergeWithdrawnContexts(contexts, Array.isArray(currentContexts) ? currentContexts : []),
  };
}
