import { normalizeAttachmentList, isImageAttachment } from './messageAttachments.js';

export const COMPOSER_CONTENT_VERSION = 1;

// Inline pasted blocks are joined to the editor text with a blank line. The C++
// normalize_composer_content (kPastedTextSeparator) applies the same rule; both
// sides are guarded by the same three sample documents in their tests.
export const PASTED_TEXT_SEPARATOR = '\n\n';

// A pasted file block uploaded from the home composer (no session yet) lives in
// the workspace draft attachment area until the first send imports it.
export const WORKSPACE_DRAFT_STORE = 'workspace_draft';
// Server-side owner name of that area; attachment records carry it as session_id.
export const WORKSPACE_DRAFT_ATTACHMENT_OWNER = '.workspace-draft';

const WORKSPACE_DRAFT_SCOPE_RE = /^[A-Za-z0-9_]{1,128}$/;
const MAX_PASTE_PARTS = 1024;
const MAX_PASTE_TITLE_CODE_POINTS = 256;

const plainText = (value) => String(value ?? '').replace(/\r\n?/g, '\n');
const string = (value) => typeof value === 'string' ? value : '';
const isPlainObject = (value) => !!value && typeof value === 'object' && !Array.isArray(value);
const nonNegativeInteger = (value) => Number.isSafeInteger(value) && value >= 0;

// Mirrors the C++ `paste` checks, but cleans instead of rejecting: the frontend
// is the producer, so an invalid field is dropped rather than losing the block.
function normalizePasteDescriptor(value) {
  if (!isPlainObject(value)) return null;
  let title = string(value.title).replace(/\0/g, '');
  if (title.length > MAX_PASTE_TITLE_CODE_POINTS) {
    // 256 code points stay below the C++ 1024 byte cap in any encoding case.
    title = Array.from(title).slice(0, MAX_PASTE_TITLE_CODE_POINTS).join('');
  }
  const paste = { title };
  if (nonNegativeInteger(value.chars)) paste.chars = value.chars;
  if (nonNegativeInteger(value.lines)) paste.lines = value.lines;
  const { part, parts } = value;
  if (Number.isSafeInteger(part) && Number.isSafeInteger(parts)
      && part >= 1 && part <= parts && parts <= MAX_PASTE_PARTS) {
    paste.part = part;
    paste.parts = parts;
  }
  return paste;
}

function workspaceDraftStoreFields(store, scope) {
  if (store !== WORKSPACE_DRAFT_STORE || !WORKSPACE_DRAFT_SCOPE_RE.test(string(scope))) return {};
  return { store: WORKSPACE_DRAFT_STORE, store_scope: scope };
}

// This contract deliberately contains no editor nodes, object URLs or upload state.
// Reject the whole unknown/malformed document: dropping a part loses user intent.
export function normalizeComposerContent(value) {
  if (!value || value.version !== COMPOSER_CONTENT_VERSION || !Array.isArray(value.parts)) return null;
  const parts = [];
  for (const part of value.parts) {
    if (!part || typeof part !== 'object' || Array.isArray(part)) return null;
    let next;
    if (part.type === 'text') {
      if (typeof part.text !== 'string') return null;
      const text = plainText(part.text);
      if (!text) continue;
      if (parts.at(-1)?.type === 'text') {
        parts.at(-1).text += text;
        continue;
      }
      next = { type: 'text', text };
    } else if (part.type === 'pasted_text') {
      // O(1) on purpose: a block may hold hundreds of KiB and normalize runs on
      // every keystroke. The text was normalized (CRLF, NUL) when it was pasted;
      // it is never rewritten or merged with neighbouring parts here.
      if (typeof part.text !== 'string') return null;
      if (part.key !== undefined && typeof part.key !== 'string') return null;
      if (!part.text) continue;
      next = { type: 'pasted_text', ...(part.key ? { key: part.key } : {}), text: part.text };
    } else if (part.type === 'path') {
      if (!string(part.path) || !string(part.token)) return null;
      next = { type: 'path', path: part.path, token: plainText(part.token) };
      if (typeof part.directory === 'boolean') next.directory = part.directory;
    } else if (part.type === 'skill') {
      if (!string(part.name) || !string(part.token)) return null;
      next = { type: 'skill', name: part.name, token: plainText(part.token) };
      if (string(part.path)) next.path = part.path;
    } else if (part.type === 'attachment') {
      if (!string(part.key) && !string(part.id)) return null;
      next = {
        type: 'attachment', key: string(part.key) || part.id,
        id: string(part.id), name: string(part.name) || 'attachment',
        kind: part.kind === 'image' ? 'image' : 'file',
      };
      if (string(part.mime_type)) next.mime_type = part.mime_type;
      if (string(part.path)) next.path = part.path;
      const paste = normalizePasteDescriptor(part.paste);
      if (paste) next.paste = paste;
      Object.assign(next, workspaceDraftStoreFields(part.store, part.store_scope));
    } else return null;
    parts.push(next);
  }
  return { version: COMPOSER_CONTENT_VERSION, parts };
}

export function cloneComposerContent(value) {
  return normalizeComposerContent(value);
}

// A paste block is either an inline `pasted_text` part or an attachment part
// carrying a `paste` descriptor (the text was stored as a file).
export function isPasteBlockPart(part) {
  return part?.type === 'pasted_text' || (part?.type === 'attachment' && isPlainObject(part.paste));
}

// Editor text only: paste blocks never enter the editor, so this stays equal to
// the Slate value (`value === composerContentText(content)`).
export function composerContentText(value) {
  return (normalizeComposerContent(value)?.parts || []).map((part) => (
    part.type === 'text' ? part.text
      : part.type === 'attachment' || part.type === 'pasted_text' ? '' : part.token
  )).join('');
}

// Joins pieces in order; a separator goes between a piece and its non-empty
// predecessor whenever either of the two is a pasted block. Empty pieces are
// skipped and do not reset the "previous was pasted" state (C++ append_submission).
function joinSubmissionPieces(pieces) {
  let output = '';
  let lastWasPaste = false;
  for (const { text, paste } of pieces) {
    if (!text) continue;
    if (output && (paste || lastWasPaste)) output += PASTED_TEXT_SEPARATOR;
    output += text;
    lastWasPaste = paste;
  }
  return output;
}

// Message body: editor text plus inline pasted blocks. File blocks are not part
// of the body; they travel as attachments and reach the model as file references.
export function composerContentSubmissionText(value) {
  const pieces = [];
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (part.type === 'attachment') continue;
    if (part.type === 'pasted_text') pieces.push({ text: part.text, paste: true });
    else pieces.push({ text: part.type === 'text' ? part.text : part.token, paste: false });
  }
  return joinSubmissionPieces(pieces);
}

// Inline pasted blocks only (file blocks are attachments and already count as extras).
export function composerContentHasPastedText(value) {
  return (normalizeComposerContent(value)?.parts || []).some((part) => part.type === 'pasted_text');
}

// The editor holds nothing but whitespace while at least one paste block
// (inline or file) exists: such input is always an ordinary message, never a command.
export function composerContentLeadsWithPastedText(value) {
  const content = normalizeComposerContent(value);
  if (!content?.parts.some(isPasteBlockPart)) return false;
  return !composerContentText(content).trim();
}

export function composerContentSignature(value) {
  const content = normalizeComposerContent(value);
  if (!content) return '';
  // Editing a keyed block always replaces its key, so key + length identifies it
  // without serializing hundreds of KiB on every signature comparison.
  return JSON.stringify({
    ...content,
    parts: content.parts.map((part) => (
      part.type === 'pasted_text' && part.key
        ? { type: part.type, key: part.key, length: part.text.length }
        : part
    )),
  });
}

export function composerAttachmentKey(attachment, index = 0) {
  return String(attachment?.local_id || attachment?.key || attachment?.id || attachment?.name || index);
}

// Workspace-draft placement follows the attachment record, not the part: a
// record owned by `.workspace-draft` is a draft file; a record owned by a real
// session has been imported; a record without an owner (rebuilt from the draft
// part itself) keeps whatever the part says.
function storeFieldsForRecord(part, record) {
  const owner = string(record?.session_id);
  if (owner === WORKSPACE_DRAFT_ATTACHMENT_OWNER) {
    return workspaceDraftStoreFields(WORKSPACE_DRAFT_STORE, string(part?.store_scope) || string(record?.store_scope));
  }
  if (owner) return {};
  return workspaceDraftStoreFields(part?.store, part?.store_scope);
}

function attachmentPart(attachment, index = 0) {
  const paste = normalizePasteDescriptor(attachment?.paste);
  return {
    type: 'attachment', key: composerAttachmentKey(attachment, index),
    id: string(attachment?.id), name: string(attachment?.name) || 'attachment',
    kind: isImageAttachment(attachment) ? 'image' : 'file',
    ...(attachment?.mime_type ? { mime_type: attachment.mime_type } : {}),
    ...(attachment?.path ? { path: attachment.path } : {}),
    ...(paste ? { paste } : {}),
    ...workspaceDraftStoreFields(attachment?.store, attachment?.store_scope),
  };
}

// 在输入框末尾追加一段文字(已有文字时先换行),粘贴块、附件、@ 引用等其它部分原样保留。
// 用于不发送、只填进输入框的场景(例如会话忙时点了对话内可视化里的追问)。
export function appendComposerText(value, text) {
  const addition = plainText(text);
  const content = normalizeComposerContent(value) || composerContentFromText('');
  if (!addition) return content;
  const separator = composerContentText(content).trim() ? '\n' : '';
  return normalizeComposerContent({
    version: content.version,
    parts: [...content.parts, { type: 'text', text: separator + addition }],
  });
}

export function composerContentFromText(text = '', attachments = []) {
  return normalizeComposerContent({
    version: COMPOSER_CONTENT_VERSION,
    parts: [...normalizeAttachmentList(attachments).map(attachmentPart), { type: 'text', text: plainText(text) }],
  });
}

function matchingAttachment(part, records) {
  return records.find((record, index) => (
    (part.id && record.id === part.id) || composerAttachmentKey(record, index) === part.key
  ));
}

export function reconcileComposerContentAttachments(value, resources = []) {
  const content = normalizeComposerContent(value);
  if (!content) return null;
  const records = normalizeAttachmentList(resources);
  return normalizeComposerContent({
    ...content,
    parts: content.parts.map((part) => {
      if (part.type !== 'attachment') return part;
      const record = matchingAttachment(part, records);
      if (!record) return part;
      const next = { ...part, ...attachmentPart({ ...part, ...record }), key: part.key };
      // The paste descriptor belongs to the part: an upload result replaces the
      // whole resource and may not carry it any more.
      const paste = part.paste || record.paste;
      if (paste) next.paste = paste;
      else delete next.paste;
      delete next.store;
      delete next.store_scope;
      return { ...next, ...storeFieldsForRecord(part, record) };
    }),
  });
}

export function composerContentAttachments(value, resources = [], { sessionId = '' } = {}) {
  const content = normalizeComposerContent(value);
  if (!content) return [];
  const records = normalizeAttachmentList(resources);
  const seen = new Set();
  const attachments = [];
  for (const part of content.parts) {
    if (part.type !== 'attachment') continue;
    const record = matchingAttachment(part, records);
    const id = string(record?.id) || part.id;
    const identity = id || part.key;
    if (seen.has(identity)) continue;
    seen.add(identity);
    const attachment = { ...part, ...record, id, local_id: part.key, type: part.kind };
    delete attachment.key;
    const paste = part.paste || record?.paste;
    if (paste) attachment.paste = paste;
    delete attachment.store;
    delete attachment.store_scope;
    const store = storeFieldsForRecord(part, record);
    Object.assign(attachment, store);
    if (store.store) {
      // A persisted draft record says /api/sessions/.workspace-draft/..., which
      // is not a servable route. Readers derive the URL from store_scope + id.
      delete attachment.blob_url;
    } else if (!attachment.blob_url && id && sessionId) {
      attachment.blob_url = `/api/sessions/${encodeURIComponent(sessionId)}/attachments/${encodeURIComponent(id)}/blob`;
    }
    // An unfinished upload can only resume while its in-memory File is alive.
    if (!id && !record) attachment.error = 'Attachment upload is unavailable';
    attachments.push(attachment);
  }
  return attachments;
}

export function composerContentFromMessage(message) {
  return normalizeComposerContent(
    message?.composerContent ?? message?.composer_content ?? message?.metadata?.composer_content,
  );
}

export function composerContentClipboardText(value) {
  const pieces = [];
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (part.type === 'text') pieces.push({ text: part.text, paste: false });
    else if (part.type === 'pasted_text') pieces.push({ text: part.text, paste: true });
    else if (part.type === 'attachment' && part.paste) {
      // The file body is not in the document; copy a readable placeholder.
      pieces.push({ text: `[粘贴的文本: ${part.paste.title || part.name}]`, paste: true });
    } else if (part.type === 'attachment') pieces.push({ text: `[${part.name}]`, paste: false });
    else pieces.push({ text: part.token, paste: false });
  }
  return joinSubmissionPieces(pieces);
}
