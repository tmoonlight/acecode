// 「粘贴的文本」块的纯逻辑(第 2 条反馈 f300:超长粘贴把整段塞进 Slate 导致卡死)。
//
// 达到折叠阈值的粘贴不进编辑器,变成输入框上方的卡片,两种形态外观一致:
//   - 内联块 {type:'pasted_text', key, text}:发送时以 "\n\n" 拼到编辑器内容之后成为正文;
//   - 文件块:文本上传成附件,composer 里是带 paste 描述的 attachment 部件,模型只收到
//     [Attached file reference],按需 file_read。
// 这里只有纯函数与常量,不碰 DOM / 网络;上传、卡片、对话框在组件层。
import {
  PASTED_TEXT_SEPARATOR,
  WORKSPACE_DRAFT_ATTACHMENT_OWNER,
  WORKSPACE_DRAFT_STORE,
  composerAttachmentKey,
  composerContentFromText,
  composerContentText,
  isPasteBlockPart,
  normalizeComposerContent,
} from './composerContent.js';
import { isComposerThumbnailAttachment } from './composerImagePresentation.js';
import { PASTED_TEXT_UPLOAD_TIMEOUT_MS, workspaceDraftAttachmentBlobPath } from './api.js';

export {
  PASTED_TEXT_SEPARATOR, PASTED_TEXT_UPLOAD_TIMEOUT_MS,
  WORKSPACE_DRAFT_ATTACHMENT_OWNER, WORKSPACE_DRAFT_STORE, isPasteBlockPart,
};

// ---- 阈值 ----------------------------------------------------------------

// 折叠阈值(用户拍板):20 行或 2000 字符,任一达到即变成粘贴块。
export const PASTED_TEXT_FOLD_MIN_LINES = 20;
export const PASTED_TEXT_FOLD_MIN_CHARS = 2000;
// 128 KiB(UTF-8)约 3~4 万 token(英文约 4 字节/token,中文约 3 字节/字)。再大
// 就不适合整段进正文:占掉大半上下文,且每轮随历史重发。达到即落文件。
export const PASTED_FILE_MIN_BYTES = 128 * 1024;
// 输入框里内联块合计上限(用户拍板 256 KiB):加入新块后超过它,新块改为文件块。
// 1 MiB 内联约 25~35 万 token,超过几乎所有模型窗口;草稿存在 meta.json,会话列表
// 要逐个解析 meta;C++ 统一 2 MiB 预算留 8 倍余量。已有内联块不回溯改形态。
export const PASTED_INLINE_TOTAL_MAX_BYTES = 256 * 1024;
// 单附件上限 25 MiB(attachment_store.hpp kMaxAttachmentBytes)留 1 MiB 余量;
// 更大的粘贴切成多个文件块,不引入输入长度上限。
export const PASTED_FILE_CHUNK_MAX_BYTES = 24 * 1024 * 1024;
// 切段时切点优先落在该段末尾这个窗口内最后一个换行之后。
export const PASTED_FILE_NEWLINE_WINDOW_BYTES = 64 * 1024;
// 超过它只提示「内容较大,将分 k 段上传」,不拦截。
export const PASTED_TEXT_LARGE_NOTICE_BYTES = 100 * 1024 * 1024;
// 实测(Chromium):原生 textarea 约 50 万字符以内按键延迟 < 60ms;200 万字符
// 110ms(中文 210~440ms)、2500 万 1.3~1.6s。超过 50 万只读,只读最多装载 500 万。
export const PASTED_TEXT_EDIT_MAX_CHARS = 500_000;
export const PASTED_TEXT_VIEW_MAX_CHARS = 5_000_000;
// 与 C++ 对齐:thread_goal_store.cpp kMaxObjectiveBytes / session_client.hpp
// kMaxSideQuestionBytes。前端只做即时反馈,服务端仍校验。
export const GOAL_OBJECTIVE_MAX_BYTES = 4000;
export const SIDE_QUESTION_MAX_BYTES = 16000;
// 对话记录里没有块信息的超长纯文本只渲染前 20000 字符 / 400 行:字符数是性能阈值
// (2 万字符 pre-wrap 排版 < 3ms),行数只为控制气泡高度。
export const LONG_USER_MESSAGE_PREVIEW_CHARS = 20000;
export const LONG_USER_MESSAGE_PREVIEW_LINES = 400;

export const PASTED_TEXT_ORIGIN = 'pasted_text';
export const NO_WORKSPACE_DRAFT_SCOPE = '__no_workspace__';
const AI_THEME_WORKSPACE_PREFIX = '__ai_theme__:';
const TITLE_SCAN_CHARS = 2048;
const SESSION_TITLE_SEED_CHARS = 200;

const string = (value) => typeof value === 'string' ? value : '';
const isPlainObject = (value) => !!value && typeof value === 'object' && !Array.isArray(value);
const nonNegativeInteger = (value) => Number.isSafeInteger(value) && value >= 0;

function truncateCodePoints(text, max) {
  const value = String(text || '');
  // 码点数 <= UTF-16 长度,长度够短时不必展开成数组。
  if (value.length <= max) return { text: value, truncated: false };
  // 每个码点最多 2 个码元:取 2*max+1 个码元必然含 >= max+1 个码点(除非已到结尾),
  // 被切开的半个代理对只会落在第 max 个码点之后,不会进结果。
  const points = Array.from(value.slice(0, max * 2 + 1));
  if (points.length <= max) return { text: value, truncated: false };
  return { text: points.slice(0, max).join(''), truncated: true };
}

// ---- 基础 ----------------------------------------------------------------

// 长度 >= 2000 时 O(1);否则逐个 indexOf 数换行,数够即停。末尾换行不算一行。
export function shouldFoldPastedText(text) {
  const value = String(text ?? '');
  if (!value) return false;
  if (value.length >= PASTED_TEXT_FOLD_MIN_CHARS) return true;
  const end = value.endsWith('\n') ? value.length - 1 : value.length;
  let newlines = 0;
  let index = value.indexOf('\n');
  while (index >= 0 && index < end) {
    newlines += 1;
    if (newlines + 1 >= PASTED_TEXT_FOLD_MIN_LINES) return true;
    index = value.indexOf('\n', index + 1);
  }
  return false;
}

export function normalizePastedText(text) {
  return String(text ?? '').replace(/\r\n?/g, '\n').replace(/\0/g, '');
}

let pasteKeySequence = 0;
export function newPasteKey() {
  pasteKeySequence += 1;
  const random = Math.random().toString(36).slice(2, 8);
  return `paste-${Date.now().toString(36)}-${pasteKeySequence.toString(36)}-${random}`;
}

export function createPastedTextPart(text, key = newPasteKey()) {
  return { type: 'pasted_text', key: String(key || newPasteKey()), text: String(text ?? '') };
}

// 卡片 / 对话框用的稳定 id:内联块用 key,文件块用部件 key(退回附件 id)。
export function pastedTextPartId(part, ordinal = 0) {
  return string(part?.key) || string(part?.id) || `paste-${ordinal}`;
}

export function pasteBlocksOf(value) {
  const content = normalizeComposerContent(value);
  if (!content) return [];
  const blocks = [];
  for (const part of content.parts) {
    if (!isPasteBlockPart(part)) continue;
    blocks.push({
      id: pastedTextPartId(part, blocks.length),
      kind: part.type === 'pasted_text' ? 'inline' : 'file',
      part,
    });
  }
  return blocks;
}

export function withoutPasteBlocks(value) {
  const content = normalizeComposerContent(value);
  if (!content) return null;
  return normalizeComposerContent({ ...content, parts: content.parts.filter((part) => !isPasteBlockPart(part)) });
}

// 编辑器投影(不含块)与块来源合并:编辑器内容在前,source 里的块按原顺序追加在后。
export function withPasteBlocksFrom(editorContent, source) {
  const editor = withoutPasteBlocks(editorContent) || composerContentFromText('');
  const blocks = (normalizeComposerContent(source)?.parts || []).filter(isPasteBlockPart);
  return normalizeComposerContent({ ...editor, parts: [...editor.parts, ...blocks] });
}

export function appendPastedTextPart(value, text, key = newPasteKey()) {
  const content = normalizeComposerContent(value) || composerContentFromText('');
  return normalizeComposerContent({ ...content, parts: [...content.parts, createPastedTextPart(text, key)] });
}

function blockMatches(part, id) {
  if (!isPasteBlockPart(part) || !id) return false;
  return part.key === id || (part.type === 'attachment' && part.id === id);
}

// 用 replacement(部件数组,可为空)替换 id 对应的块,位置不变;找不到时原样返回。
export function replacePasteBlock(value, id, replacement = []) {
  const content = normalizeComposerContent(value);
  if (!content) return null;
  const index = content.parts.findIndex((part) => blockMatches(part, id));
  if (index < 0) return content;
  const parts = [...content.parts];
  parts.splice(index, 1, ...Array.from(replacement || []));
  return normalizeComposerContent({ ...content, parts });
}

// 编辑内联块:新文本换新 key(签名只看 key + 长度,不换 key 会漏判修改);空文本即删除。
export function replacePastedTextPart(value, id, text, key = newPasteKey()) {
  const next = String(text ?? '');
  return replacePasteBlock(value, id, next ? [createPastedTextPart(next, key)] : []);
}

export function removePastedTextPart(value, id) {
  return replacePasteBlock(value, id, []);
}

// 顶层 payload.text:编辑器文本(已做会话引用等转换)之后依次拼上内联块。块总在
// 编辑器内容之后,所以这与 composerContentSubmissionText 的分隔规则逐字节一致。
export function appendPastedTextToSubmission(base, value) {
  let output = String(base ?? '');
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (part.type !== 'pasted_text' || !part.text) continue;
    if (output) output += PASTED_TEXT_SEPARATOR;
    output += part.text;
  }
  return output;
}

// 只看前 2KB:第一条非空行,压缩空白,按码点截断。
export function pastedTextTitle(text, max = 60) {
  let head = String(text ?? '').slice(0, TITLE_SCAN_CHARS);
  const last = head.charCodeAt(head.length - 1);
  if (last >= 0xD800 && last <= 0xDBFF) head = head.slice(0, -1);
  for (const line of head.split('\n')) {
    const compact = line.replace(/\s+/g, ' ').trim();
    if (!compact) continue;
    const { text: title, truncated } = truncateCodePoints(compact, max);
    return truncated ? `${title}…` : title;
  }
  return '粘贴的文本';
}

// chars = Unicode 码点数(与 utf8RangeStats 同口径);lines 不计末尾换行,空串为 0。
export function pastedTextStats(text) {
  const value = String(text ?? '');
  let chars = value.length;
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code >= 0xD800 && code <= 0xDBFF && (value.charCodeAt(index + 1) & 0xFC00) === 0xDC00) {
      chars -= 1;
      index += 1;
    }
  }
  if (!value) return { chars: 0, lines: 0 };
  let newlines = 0;
  for (let index = value.indexOf('\n'); index >= 0; index = value.indexOf('\n', index + 1)) newlines += 1;
  return { chars, lines: newlines + (value.endsWith('\n') ? 0 : 1) };
}

// 字节区间 [start, end) 的统计,口径同 pastedTextStats(切段后不必再解码)。
export function utf8RangeStats(bytes, start = 0, end = bytes?.length || 0) {
  let chars = 0;
  let newlines = 0;
  for (let index = start; index < end; index += 1) {
    const byte = bytes[index];
    if ((byte & 0xC0) !== 0x80) chars += 1;
    if (byte === 0x0A) newlines += 1;
  }
  if (end <= start) return { chars: 0, lines: 0 };
  return { chars, lines: newlines + (bytes[end - 1] === 0x0A ? 0 : 1) };
}

// 旧长文本入框(没有 composer_content 的旧草稿、从旧消息 fork 回填、上箭头翻到旧历史)
// 是否要整段走粘贴块分类。结构化内容只在「纯文本且 >= 2 万字符」时才折叠:本版之前
// 大段粘贴也会存成一个 text 部件,而手打到 2 万字符几乎不会发生;带路径 / 技能 / 附件
// 等引用的内容永不折叠,以免丢结构。
export function legacyTextNeedsFold(text, content = null) {
  const value = String(text ?? '');
  const normalized = normalizeComposerContent(content);
  if (!normalized) return shouldFoldPastedText(value);
  if (!normalized.parts.every((part) => part.type === 'text')) return false;
  return composerContentText(normalized).length >= LONG_USER_MESSAGE_PREVIEW_CHARS;
}

// 旧草稿 / fork 回填的超长文本折叠成文件块后,在这些块拿到服务端 id 之前,服务端草稿里
// 的旧全文是它唯一的持久副本(页面内存里的 File 刷新即丢)。guard = { localIds } 记下这次
// 折叠暂存的块;content 必须是已按资源回填 id 的内容(reconcileComposerContentAttachments /
// composerDraftSnapshot 的结果)。返回 true = 仍有折叠块留在内容里且没有 id,此时不能用
// 「空文本 + 无 id 的附件部件」覆盖草稿。块被用户删掉或全部上传完成即返回 false。
export function legacyFoldUploadPending(guard, content) {
  const ids = Array.isArray(guard?.localIds) ? guard.localIds : [];
  if (ids.length === 0) return false;
  const normalized = normalizeComposerContent(content);
  return (normalized?.parts || []).some((part) => (
    part.type === 'attachment' && ids.includes(part.key) && !part.id
  ));
}

// cwd 输入历史只记编辑器文本:payload.text 里拼上的内联块正文不进历史。
export function inputHistoryTextForPayload(payload) {
  const text = String(payload?.text ?? '');
  const content = normalizeComposerContent(payload?.composer_content);
  if (!content) return text;
  const inline = content.parts.filter((part) => part.type === 'pasted_text').map((part) => part.text);
  if (!inline.length) return text;
  const tail = inline.join(PASTED_TEXT_SEPARATOR);
  if (text === tail) return '';
  if (text.endsWith(`${PASTED_TEXT_SEPARATOR}${tail}`)) {
    return text.slice(0, text.length - PASTED_TEXT_SEPARATOR.length - tail.length);
  }
  return composerContentText(content);
}

function pasteBlockTitle(part) {
  if (part?.type === 'pasted_text') return pastedTextTitle(part.text);
  return string(part?.paste?.title) || string(part?.name);
}

// 首页新建会话的标题种子:编辑器文本截 200 字符,否则第一个粘贴块的标题。
export function sessionTitleSeedForPayload(payload) {
  const editor = inputHistoryTextForPayload(payload).trim();
  if (editor) return truncateCodePoints(editor, SESSION_TITLE_SEED_CHARS).text;
  const content = normalizeComposerContent(payload?.composer_content);
  const first = content?.parts.find(isPasteBlockPart);
  return first ? pasteBlockTitle(first) : '';
}

// 排队卡片等只需要概览的地方:两种块都显示为 [label],结果最多 maxChars 字符。
export function composerContentDisplayText(value, label = '粘贴的文本', { maxChars = 4096 } = {}) {
  let output = '';
  let lastWasPaste = false;
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (output.length >= maxChars) break;
    const paste = isPasteBlockPart(part);
    let text;
    if (paste) text = `[${label}]`;
    else if (part.type === 'text') text = part.text;
    else if (part.type === 'attachment') text = '';
    else text = part.token;
    if (!text) continue;
    if (output && (paste || lastWasPaste)) output += PASTED_TEXT_SEPARATOR;
    output += text.slice(0, Math.max(0, maxChars - output.length));
    lastWasPaste = paste;
  }
  return output.slice(0, maxChars);
}

// ---- 资源判定 ------------------------------------------------------------

// 文件块部件的 key 与 id 集合(资源上的临时标记丢失时,靠它认出粘贴资源)。
export function pasteResourceKeys(value) {
  const keys = new Set();
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (part.type !== 'attachment' || !isPasteBlockPart(part)) continue;
    if (part.key) keys.add(part.key);
    if (part.id) keys.add(part.id);
  }
  return keys;
}

// 「是不是粘贴块资源」不能只看资源上的临时字段:上传回填会用服务端记录整体替换
// 资源(paste 标记随之丢失),部件被删后 key 集合也不再包含它。任一条件成立即算:
// 资源 paste 描述 / 服务端 metadata.origin / File 上登记的描述 / 部件 key 或 id。
export function isPasteResource(resource, pasteKeys = null) {
  if (!resource || typeof resource !== 'object') return false;
  if (isPlainObject(resource.paste)) return true;
  if (resource.metadata?.origin === PASTED_TEXT_ORIGIN || resource.origin === PASTED_TEXT_ORIGIN) return true;
  if (pastedTextFileMeta(resource.file)) return true;
  const keys = pasteKeys instanceof Set ? pasteKeys : new Set(Array.from(pasteKeys || []));
  if (!keys.size) return false;
  return [resource.local_id, resource.key, resource.id].some((value) => value && keys.has(String(value)));
}

// 交给 RichComposer 的附件:剥掉图片(缩略图条)与粘贴资源(卡片条)。否则 RichComposer
// 会把「没见过、文档里也没有」的资源插成编辑器里的内联附件标签。
export function editorAttachmentResources(attachments = [], composerContent = null) {
  const keys = pasteResourceKeys(composerContent);
  return Array.from(attachments || []).filter((item) => (
    !isComposerThumbnailAttachment(item) && !isPasteResource(item, keys)
  ));
}

// ---- File 上的描述 -------------------------------------------------------

// 描述跟着 File 走:保留项、草稿恢复时重新保留、首页发送的待上传列表都持有同一个
// File,任何路径都能取回,不依赖保留项对象本身。
const pastedTextFiles = new WeakMap();

function cleanPasteDescriptor(paste) {
  if (!isPlainObject(paste)) return null;
  const clean = { title: string(paste.title) };
  if (nonNegativeInteger(paste.chars)) clean.chars = paste.chars;
  if (nonNegativeInteger(paste.lines)) clean.lines = paste.lines;
  if (Number.isSafeInteger(paste.part) && Number.isSafeInteger(paste.parts)
      && paste.part >= 1 && paste.part <= paste.parts) {
    clean.part = paste.part;
    clean.parts = paste.parts;
  }
  return clean;
}

export function registerPastedTextFile(file, paste) {
  const clean = cleanPasteDescriptor(paste);
  if (file && typeof file === 'object' && clean) pastedTextFiles.set(file, clean);
  return file;
}

export function pastedTextFileMeta(file) {
  if (!file || typeof file !== 'object') return null;
  return pastedTextFiles.get(file) || null;
}

// ---- 分类与切段 ----------------------------------------------------------

// UTF-8 字节数;超过 limit 即停止并返回一个 > limit 的值(不保证精确)。
// 每个 UTF-16 码元至少 1 字节,所以长度 > limit 时不必扫描。孤立代理项按
// TextEncoder 的替换字符 U+FFFD 计 3 字节。
export function utf8ByteLengthBounded(text, limit = Number.MAX_SAFE_INTEGER) {
  const value = String(text ?? '');
  if (value.length > limit) return value.length;
  let bytes = 0;
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code < 0x80) bytes += 1;
    else if (code < 0x800) bytes += 2;
    else if (code >= 0xD800 && code <= 0xDBFF && (value.charCodeAt(index + 1) & 0xFC00) === 0xDC00) {
      bytes += 4;
      index += 1;
    } else bytes += 3;
    if (bytes > limit) return bytes;
  }
  return bytes;
}

// 输入框里已有内联块的 UTF-8 字节合计;超过 limit 即返回。
export function inlinePastedBytes(value, limit = PASTED_INLINE_TOTAL_MAX_BYTES) {
  let total = 0;
  for (const part of normalizeComposerContent(value)?.parts || []) {
    if (part.type !== 'pasted_text') continue;
    total += utf8ByteLengthBounded(part.text, limit - total);
    if (total > limit) return total;
  }
  return total;
}

// 按字节切段,每段 <= maxBytes;切点优先落在该段末尾 newlineWindow 内最后一个换行
// 之后,否则退到 UTF-8 字符起点(不切开多字节字符)。区间首尾相接、覆盖全部字节。
export function splitUtf8Ranges(bytes, maxBytes = PASTED_FILE_CHUNK_MAX_BYTES,
  newlineWindow = PASTED_FILE_NEWLINE_WINDOW_BYTES) {
  const length = bytes?.length || 0;
  const ranges = [];
  let start = 0;
  while (start < length) {
    if (length - start <= maxBytes) {
      ranges.push({ start, end: length });
      break;
    }
    const limit = start + maxBytes;
    const floor = Math.max(start, limit - newlineWindow);
    const newline = bytes.lastIndexOf(0x0A, limit - 1);
    let end;
    if (newline >= floor) end = newline + 1;
    else {
      end = limit;
      while (end > start && (bytes[end] & 0xC0) === 0x80) end -= 1;
      if (end === start) end = limit; // maxBytes 小于一个字符时不会发生在真实阈值上
    }
    ranges.push({ start, end });
    start = end;
  }
  return ranges;
}

// 粘贴一段文本时的去向(text 须已经过 normalizePastedText):
//   {kind:'plain'}                      未达折叠阈值,照常进编辑器;
//   {kind:'inline', text, bytes}        内联块;
//   {kind:'file', bytes, byteLength, notice, chunks}
//     UTF-8 >= 128 KiB,或「已有内联块合计 + 本块 > 256 KiB」。bytes 是整段的一次
//     TextEncoder 编码结果,chunks 为字节区间 [start,end) 及其 paste 描述;> 24 MiB
//     时多段,各段带 part/parts,标题加「(k/n)」。notice 表示 > 100 MiB 应提示一次。
export function planPastedTextInsertion(content, text) {
  const value = String(text ?? '');
  if (!shouldFoldPastedText(value)) return { kind: 'plain' };
  const size = utf8ByteLengthBounded(value, PASTED_FILE_MIN_BYTES);
  if (size < PASTED_FILE_MIN_BYTES) {
    const existing = inlinePastedBytes(content, PASTED_INLINE_TOTAL_MAX_BYTES);
    if (existing + size <= PASTED_INLINE_TOTAL_MAX_BYTES) return { kind: 'inline', text: value, bytes: size };
  }
  const bytes = new TextEncoder().encode(value);
  const ranges = bytes.length > PASTED_FILE_CHUNK_MAX_BYTES
    ? splitUtf8Ranges(bytes, PASTED_FILE_CHUNK_MAX_BYTES)
    : [{ start: 0, end: bytes.length }];
  const baseTitle = pastedTextTitle(value);
  const parts = ranges.length;
  const chunks = ranges.map((range, index) => {
    const stats = utf8RangeStats(bytes, range.start, range.end);
    const multi = parts > 1;
    const title = multi ? `${baseTitle}（${index + 1}/${parts}）` : baseTitle;
    const paste = { title, chars: stats.chars, lines: stats.lines, ...(multi ? { part: index + 1, parts } : {}) };
    return { ...range, title, ...(multi ? { part: index + 1, parts } : {}), paste };
  });
  return {
    kind: 'file', bytes, byteLength: bytes.length,
    notice: bytes.length > PASTED_TEXT_LARGE_NOTICE_BYTES, chunks,
  };
}

let lastFileStamp = '';
let fileStampSerial = 0;
const pad = (value, width = 2) => String(value).padStart(width, '0');

// pasted_text_YYYYMMDD_HHMMSS[_s][_k].txt:ASCII、同一页面内唯一。
// 同一秒内的第二次粘贴加 _2、_3…(否则 composerFileIdentity 会把两次相同粘贴去重);
// 多段时 index 为 1..n 的段号,单段传 0。一次粘贴的各段须传同一个 now,且从
// index 0 或 1 开始按顺序调用(index <= 1 视为新的一次粘贴)。
export function pastedTextFileName(now = new Date(), index = 0) {
  const date = now instanceof Date ? now : new Date(now);
  const stamp = `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}`
    + `_${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
  if (index <= 1) {
    if (stamp === lastFileStamp) fileStampSerial += 1;
    else {
      lastFileStamp = stamp;
      fileStampSerial = 1;
    }
  }
  const serial = fileStampSerial > 1 ? `_${fileStampSerial}` : '';
  const chunk = index > 0 ? `_${index}` : '';
  return `pasted_text_${stamp}${serial}${chunk}.txt`;
}

// 上传请求体里的来源描述,服务端存进附件记录 metadata(origin / pasted_text)。
export function pastedTextUploadBody(paste) {
  const clean = cleanPasteDescriptor(paste) || { title: '' };
  const { title: _title, ...counts } = clean;
  return { origin: PASTED_TEXT_ORIGIN, paste: counts };
}

// ---- 首页草稿 ------------------------------------------------------------

// 首页粘贴文件块的存放 scope:'' → __no_workspace__;AI 主题等临时首页(本来就不落盘)
// 返回 '' 表示不上传,留给发送时的现有流程。
export function homeDraftAttachmentScope(workspaceHash = '') {
  const hash = String(workspaceHash || '');
  if (hash.startsWith(AI_THEME_WORKSPACE_PREFIX)) return '';
  return hash || NO_WORKSPACE_DRAFT_SCOPE;
}

// payload 里仍指向工作区草稿附件区的引用 → [{id, workspace}],按 id 去重。
export function workspaceDraftPasteRefs(payload) {
  const refs = [];
  const seen = new Set();
  const add = (id, workspace) => {
    const key = String(id || '');
    if (!key || !workspace || seen.has(key)) return;
    seen.add(key);
    refs.push({ id: key, workspace: String(workspace) });
  };
  for (const item of Array.from(payload?.attachments || [])) {
    if (item?.store === WORKSPACE_DRAFT_STORE) add(item.id, item.store_scope);
  }
  for (const part of normalizeComposerContent(payload?.composer_content)?.parts || []) {
    if (part.type === 'attachment' && part.store === WORKSPACE_DRAFT_STORE) add(part.id, part.store_scope);
  }
  return refs;
}

// imported: [{id: 旧的草稿附件 id, key?, attachment: 导入后的会话附件记录}]。
// 按 key / 旧 id 替换成新 id,并清掉 store / store_scope(发出去的一定是会话附件)。
export function payloadWithImportedPastes(payload, imported = []) {
  const entries = Array.from(imported || []).filter((entry) => entry?.attachment?.id && (entry.id || entry.key));
  if (!payload || !entries.length) return payload;
  const byId = new Map(entries.filter((entry) => entry.id).map((entry) => [String(entry.id), entry]));
  const byKey = new Map(entries.filter((entry) => entry.key).map((entry) => [String(entry.key), entry]));
  const seen = new Set();
  const attachments = [];
  for (const item of Array.from(payload.attachments || [])) {
    const entry = item?.store === WORKSPACE_DRAFT_STORE ? byId.get(String(item.id || '')) : null;
    const next = entry ? { id: entry.attachment.id } : item;
    const identity = String(next?.id || '');
    if (identity && seen.has(identity)) continue;
    if (identity) seen.add(identity);
    attachments.push(next);
  }
  const result = { ...payload, attachments };
  const content = normalizeComposerContent(payload.composer_content);
  if (content) {
    result.composer_content = normalizeComposerContent({
      ...content,
      parts: content.parts.map((part) => {
        if (part.type !== 'attachment') return part;
        const entry = (part.id && byId.get(part.id)) || byKey.get(part.key);
        if (!entry) return part;
        const next = { ...part, id: String(entry.attachment.id) };
        delete next.store;
        delete next.store_scope;
        return next;
      }),
    });
  }
  return result;
}

// 离开首页后才完成的草稿附件上传,回填到 store 里的最新草稿。草稿里已没有该块
// (已发送或已删除)时返回 null,调用方不写;否则回填 id + store + store_scope,并用
// 上传结果替换 draft.attachments 里的本地资源。
export function reconcileHomeDraftUpload(draft, uploadedItem, scope) {
  const content = normalizeComposerContent(draft?.composer_content);
  const key = String(uploadedItem?.local_id || '');
  const id = string(uploadedItem?.id);
  const storeScope = String(scope || '');
  if (!content || !key || !id || !storeScope) return null;
  const matches = (part) => part.type === 'attachment' && part.key === key;
  if (!content.parts.some(matches)) return null;
  const composer_content = normalizeComposerContent({
    ...content,
    parts: content.parts.map((part) => (
      matches(part) ? { ...part, id, store: WORKSPACE_DRAFT_STORE, store_scope: storeScope } : part
    )),
  });
  const resource = { ...uploadedItem, store: WORKSPACE_DRAFT_STORE, store_scope: storeScope };
  const attachments = Array.from(draft?.attachments || []);
  const index = attachments.findIndex((item, position) => (
    composerAttachmentKey(item, position) === key || item?.local_id === key
  ));
  if (index >= 0) attachments[index] = resource;
  else attachments.push(resource);
  return { ...draft, composer_content, attachments };
}

// ---- 命令 ----------------------------------------------------------------

// /goal、/btw|/side 带粘贴块时:有文件块、或并入参数后超过服务端上限 → 返回
// {command, limitBytes},前端提示「太长」并保留输入框;未超限返回 null,照常并入参数。
// 行为随大小单调变化:不会因为跨过 128 KiB 就从「创建 goal」翻成「发一条字面消息」。
export function pasteTooLongForCommand(route, payload) {
  const content = normalizeComposerContent(payload?.composer_content);
  const blocks = content ? content.parts.filter(isPasteBlockPart) : [];
  if (!blocks.length) return null;
  let command;
  let limitBytes;
  let argument;
  if (route?.kind === 'builtin' && String(route.command?.command || '').toLowerCase() === 'goal') {
    command = 'goal';
    limitBytes = GOAL_OBJECTIVE_MAX_BYTES;
    argument = route.command.args;
  } else if (route?.kind === 'side_question') {
    command = String(route.command || 'btw');
    limitBytes = SIDE_QUESTION_MAX_BYTES;
    argument = route.question;
  } else return null;
  if (blocks.some((part) => part.type === 'attachment')) return { command, limitBytes };
  return utf8ByteLengthBounded(String(argument || ''), limitBytes) > limitBytes ? { command, limitBytes } : null;
}

// ---- 查看来源 ------------------------------------------------------------

// 打开卡片时的文本来源:{text} | {file} | {url} | null。
// item 可以是 pasteBlocksOf 的块({kind, part}),也可以附带 resource / file / blob_url。
// 工作区草稿附件一律由 store_scope + id 计算 URL,不读记录里的 blob_url
// (持久化的是 /api/sessions/.workspace-draft/…,不是可用路由)。
export function pasteBlockTextSource(item, { sessionId = '' } = {}) {
  const part = item?.part || item;
  if (part?.type === 'pasted_text') return { text: part.text };
  const resource = item?.resource || null;
  const file = item?.file || resource?.file;
  if (file) return { file };
  const id = string(part?.id) || string(resource?.id);
  const draftOwned = string(resource?.session_id) === WORKSPACE_DRAFT_ATTACHMENT_OWNER;
  const store = part?.store || resource?.store || (draftOwned ? WORKSPACE_DRAFT_STORE : '');
  const scope = string(part?.store_scope) || string(resource?.store_scope);
  if (store === WORKSPACE_DRAFT_STORE) {
    return id && scope ? { url: workspaceDraftAttachmentBlobPath(scope, id) } : null;
  }
  const blobUrl = string(item?.blob_url) || string(resource?.blob_url);
  if (blobUrl) return { url: blobUrl };
  if (id && sessionId) {
    return { url: `/api/sessions/${encodeURIComponent(sessionId)}/attachments/${encodeURIComponent(id)}/blob` };
  }
  return null;
}
