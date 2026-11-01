import { extractSessionReferences } from './sessionReference.js';
import { reconcileComposerContentAttachments, WORKSPACE_DRAFT_STORE } from './composerContent.js';
import { appendPastedTextToSubmission } from './pastedText.js';
import { normalizeComposerContext } from './selectionChatContext.js';

export function normalizeComposerPayload(text, attachments = [], contexts = [], swarmMode = null, composerContent = null) {
  const sessionReferences = extractSessionReferences(String(text || ''));
  const content = reconcileComposerContentAttachments(composerContent, attachments);
  const payload = {
    // 消息正文 = 编辑器文本 + 内联粘贴块(以 "\n\n" 拼在编辑器内容之后);文件块不进
    // 正文,走 attachments → [Attached file reference]。
    text: appendPastedTextToSubmission(sessionReferences.displayText, content),
    // 首页草稿附件(粘贴的文件块)留在 attachments 里并带 store / store_scope:这样
    // payloadHasExtras 原样就把「只有文件块」「/compact + 文件块」算作 extras;发送前
    // 由 materializeWorkspaceDraftPastes 这一个导入点复制成会话附件。
    attachments: attachments
      .filter((item) => item && !item.uploading && item.id)
      .map((item) => (item.store === WORKSPACE_DRAFT_STORE && item.store_scope
        ? { id: item.id, store: item.store, store_scope: item.store_scope }
        : { id: item.id })),
    contexts: contexts.map(normalizeComposerContext).filter(Boolean),
  };
  if (sessionReferences.references.length > 0) {
    payload.session_references = sessionReferences.references;
  }
  if (swarmMode) payload.swarm_mode = swarmMode;
  if (content) payload.composer_content = content;
  return payload;
}
