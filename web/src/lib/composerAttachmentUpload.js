import { ATTACHMENT_HARD_LIMIT_BYTES, normalizeImageFile } from './imageNormalize.js';
import { fileSourcePath, fileSourceReference } from './composerFileTransfer.js';
import { pastedTextFileMeta, pastedTextUploadBody, PASTED_TEXT_UPLOAD_TIMEOUT_MS } from './pastedText.js';

export async function uploadComposerAttachment(api, sessionId, file) {
  const reference = fileSourceReference(file);
  const sourcePath = reference?.sourcePath || fileSourcePath(file);
  if (reference) return api.createSessionAttachmentReference(sessionId, {
    name: file.name || 'attachment', mime_type: file.type || '',
    source_path: reference.sourcePath, reference_only: true,
  });
  const { file: uploadFile } = await normalizeImageFile(file);
  if (uploadFile.size > ATTACHMENT_HARD_LIMIT_BYTES) throw new Error('附件超过 25MiB，且无法压缩到限制内');
  const data = await new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onerror = () => reject(reader.error || new Error('文件读取失败'));
    reader.onload = () => resolve(String(reader.result || '').split(',')[1] || '');
    reader.readAsDataURL(uploadFile);
  });
  const paste = pastedTextFileMeta(file);
  return api.uploadSessionAttachment(sessionId, {
    name: uploadFile.name || file.name || 'attachment',
    mime_type: uploadFile.type || file.type || '', data_base64: data,
    ...(sourcePath ? { source_path: sourcePath } : {}),
    ...(paste ? pastedTextUploadBody(paste) : {}),
  }, paste ? { timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS } : {});
}
