import { composerContentFromText, composerContentText, normalizeComposerContent, reconcileComposerContentAttachments } from './composerContent.js';
import { removeComposerAttachmentReference } from './composerDraft.js';
import { reserveComposerAttachmentFiles, createComposerAttachmentReservations, clearComposerAttachmentReservations } from './composerAttachmentReservations.js';
import { isComposerThumbnailAttachment } from './composerImagePresentation.js';
import { appendPastedTextPart, planPastedTextInsertion, registerPastedTextFile, pastedTextFileName, pastedTextFileMeta, replacePasteBlock } from './pastedText.js';

// The lifetime belongs to the side controller, not a mounted editor. Uploads
// may finish while hidden; clear/expiry invalidate them before releasing URLs.
export function createSideChatDraft({ read, publish, uploadAttachment }) {
  const reservations = createComposerAttachmentReservations();
  const urls = new Set();
  let generation = 0;
  const content = () => read().composerContent || composerContentFromText(read().draft);
  const setContent = next => publish({ composerContent: next, draft: composerContentText(next) });
  async function upload(item) {
    const epoch = generation;
    const valid = () => generation === epoch && read().attachments.some(resource => resource.local_id === item.local_id);
    if (!valid()) return;
    publish({ attachments: read().attachments.map(resource => resource.local_id === item.local_id
      ? { ...resource, uploading: true, upload_error: '' } : resource) });
    try {
      const result = await uploadAttachment(item.file);
      if (!valid()) return;
      if (!result?.attachment?.id) throw new Error('附件上传失败');
      const uploaded = { ...item, ...result.attachment, local_id: item.local_id, uploading: false, upload_error: '' };
      publish({
        attachments: read().attachments.map(resource => resource.local_id === item.local_id ? uploaded : resource),
        composerContent: reconcileComposerContentAttachments(content(), [uploaded]),
      });
    } catch (error) {
      if (valid()) publish({ attachments: read().attachments.map(resource => resource.local_id === item.local_id
        ? { ...resource, uploading: false, upload_error: String(error?.message || '附件上传失败') } : resource) });
    }
  }
  function addFiles(files, replaceId = '') {
    const items = reserveComposerAttachmentFiles(reservations, files).map(({ file, localId }) => {
      const item = { file, local_id: localId, name: file.name, mime_type: file.type,
        kind: /^image\//.test(file.type) ? 'image' : 'file', size_bytes: file.size, uploading: true,
        ...(pastedTextFileMeta(file) ? { paste: pastedTextFileMeta(file) } : {}) };
      if (isComposerThumbnailAttachment(item) && globalThis.URL?.createObjectURL) {
        item.preview_url = URL.createObjectURL(file);
        urls.add(item.preview_url);
      }
      return item;
    });
    if (!items.length) return;
    const parts = composerContentFromText('', items).parts;
    const next = replaceId ? replacePasteBlock(content(), replaceId, parts)
      : normalizeComposerContent({ ...content(), parts: [...content().parts, ...parts] });
    publish({ attachments: [...read().attachments, ...items], composerContent: next, draft: composerContentText(next) });
    // Preserve gesture order for large text chunks; failures retain retryable resources.
    return items.reduce((promise, item) => promise.then(() => upload(item)), Promise.resolve());
  }
  return {
    addFiles,
    retry(key) {
      const item = read().attachments.find(resource => resource.local_id === key || resource.id === key);
      if (item?.file && !item.uploading) return upload(item);
    },
    remove(key) {
      // Keep resources/reservations for editor undo, just like the main draft.
      setContent(removeComposerAttachmentReference(content(), key));
    },
    paste(text, replaceId = '') {
      const plan = planPastedTextInsertion(content(), text);
      if (plan.kind === 'plain' && !replaceId) return false;
      if (plan.kind !== 'file') {
        const added = appendPastedTextPart(composerContentFromText(''), text).parts;
        setContent(replaceId ? replacePasteBlock(content(), replaceId, added) : appendPastedTextPart(content(), text));
      } else {
        addFiles(plan.chunks.map((chunk, index) => registerPastedTextFile(
          new File([plan.bytes.subarray(chunk.start, chunk.end)], pastedTextFileName(new Date(), plan.chunks.length > 1 ? index + 1 : 0), { type: 'text/plain' }), chunk.paste,
        )), replaceId);
      }
      return true;
    },
    submitted() { clearComposerAttachmentReservations(reservations); },
    destroy() {
      generation++;
      clearComposerAttachmentReservations(reservations);
      for (const url of urls) URL.revokeObjectURL(url);
      urls.clear();
    },
  };
}
