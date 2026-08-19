// 把 queue item 翻译成 QueueCardList 卡片要展示的结构。
// 抽出来是为了让 Node 单测能不依赖 DOM 验证状态/标签的映射。
//
// 输入是 chatInputQueue.js::buildQueuedMessageItems 返回的 item:
//   { kind:'msg', id, content, ts, queued: { id, sessionId, state, error, ... } }
// 输出是给 QueueCardList.jsx 一个稳定的 props 形状。

import { QUEUED_INPUT_STATE } from './chatInputQueue.js';

export function buildQueueCardItem(item) {
  const queued = item?.queued || {};
  const payload = queued.payload || {};
  const attachmentCount = Array.isArray(payload.attachments) ? payload.attachments.length : 0;
  const contextCount = Array.isArray(payload.contexts) ? payload.contexts.length : 0;
  const fallbackContent = [
    attachmentCount ? `${attachmentCount} 个附件` : '',
    contextCount ? `${contextCount} 个上下文` : '',
  ].filter(Boolean).join(' + ');
  const state = queued.state || QUEUED_INPUT_STATE.QUEUED;
  let statusLabel = '排队中';
  let statusKind = 'queued';
  let dimmed = false;
  let showRetry = false;
  if (state === QUEUED_INPUT_STATE.SENDING) {
    statusLabel = '发送中…';
    statusKind = 'sending';
    dimmed = true;
  } else if (state === QUEUED_INPUT_STATE.FAILED) {
    statusLabel = String(queued.error || '发送失败');
    statusKind = 'failed';
    showRetry = true;
  } else if (state === QUEUED_INPUT_STATE.GUIDING) {
    statusLabel = queued.acceptedAt ? '正在打断当前回合…' : '正在提交插话…';
    statusKind = 'guiding';
    dimmed = true;
  }
  const rawContent = String(item?.content || '');
  const hasText = rawContent.trim().length > 0;
  const hasExtras = attachmentCount > 0 || contextCount > 0;
  const canEdit = (hasText || hasExtras) &&
    (state === QUEUED_INPUT_STATE.QUEUED || state === QUEUED_INPUT_STATE.FAILED);
  const canGuide = canEdit;
  return {
    queuedId: queued.id || '',
    content: String(rawContent || fallbackContent || ''),
    editText: rawContent,
    hasExtras,
    state,
    statusLabel,
    statusKind,
    dimmed,
    showRetry,
    canEdit,
    canGuide,
  };
}

export function buildQueueCardItems(items) {
  return (Array.isArray(items) ? items : []).map(buildQueueCardItem);
}
