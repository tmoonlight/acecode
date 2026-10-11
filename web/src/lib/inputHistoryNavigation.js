import { composerContentFromMessage, composerContentSignature, composerContentText } from './composerContent.js';
import { isShortcut } from './shortcutBindings.js';

// hasNonTextContent:输入框里还有粘贴块 / 附件等不在编辑器文本里的内容。此时编辑器
// 为空不代表输入框为空,上箭头不能进入历史(否则几 MB 的粘贴会被历史条目静默替换);
// 已经在翻历史(historyPointer >= 0 且未编辑)时照常继续翻。
// 不在翻历史(historyPointer < 0)时只要有非文本内容就一律不进入 —— 不能只靠
// editedSinceHistory:粘贴块、恢复的草稿、旧长文本折叠都不经编辑器 onChange,
// editedSinceHistory 可能仍是发送后复位的 false。
export function isInputHistoryNavigationMode({
  value = '', editedSinceHistory = false, hasNonTextContent = false, historyPointer = -1,
} = {}) {
  if (hasNonTextContent && !(Number.isInteger(historyPointer) && historyPointer >= 0)) return false;
  return (String(value ?? '').length === 0 && !hasNonTextContent) || !editedSinceHistory;
}

// 富文本编辑器在程序化设值(历史导航填充 / 外部 value 同步)和 selection-only
// 变化时也可能触发 onChange 回声,此时文本与当前 value 相同。这类回声不是用户编辑,
// 不能把 editedSinceHistory 置 true,否则历史浏览态一填充就被打断。
export function isUserComposerEdit({ nextValue = '', currentValue = '' } = {}) {
  return String(nextValue ?? '') !== String(currentValue ?? '');
}

// 从 transcript item 提取用户原始输入文本。skill 命令等场景下落库的 content 是
// 展开后的注入提示,原文存在 metadata.display_text,翻历史时必须优先取原文。
export function userInputTextFromTranscriptItem(item) {
  if (item?.kind !== 'msg' || item?.role !== 'user') return '';
  const displayText = item?.metadata?.display_text;
  if (typeof displayText === 'string' && displayText.trim()) return displayText;
  return String(item?.content || '');
}

const MAX_COMPOSER_HISTORY = 500;

export function buildComposerHistoryEntries({ cwdHistory = [], transcriptItems = [] } = {}) {
  const merged = Array.from(cwdHistory || []).map((text) => ({ text: String(text || '') }));
  for (const item of transcriptItems || []) {
    if (item?.kind !== 'msg' || item.role !== 'user') continue;
    const composer_content = composerContentFromMessage(item);
    merged.push({
      text: composer_content ? composerContentText(composer_content) : userInputTextFromTranscriptItem(item),
      ...(composer_content ? { composer_content } : {}),
    });
  }
  const seen = new Set();
  return merged.filter((entry) => entry.text.trim() || entry.composer_content?.parts?.length)
    .reverse().filter((entry) => {
      // Legacy text records are superseded by a matching structured transcript entry.
      const key = entry.composer_content ? `${entry.text}\u0000${composerContentSignature(entry.composer_content)}` : entry.text;
      if (seen.has(key)) return false;
      seen.add(key);
      if (entry.composer_content) seen.add(entry.text);
      return true;
    }).slice(0, MAX_COMPOSER_HISTORY).reverse();
}

// 上下键翻的历史 = per-cwd 输入历史(与 TUI 共享,受 max_entries 截断)+ 当前
// transcript 会话中用户发过的消息。会话消息排在尾部,↑ 优先翻到当前会话的输入,
// 翻完再进入 per-cwd 更早的历史;同文本去重保留最后一次出现。
export function buildComposerHistory({ cwdHistory = [], transcriptItems = [] } = {}) {
  const cwdEntries = Array.isArray(cwdHistory) ? cwdHistory : [];
  const items = Array.isArray(transcriptItems) ? transcriptItems : [];
  const merged = [];
  for (const entry of cwdEntries) {
    const text = String(entry ?? '');
    if (text.trim()) merged.push(text);
  }
  for (const item of items) {
    const text = userInputTextFromTranscriptItem(item);
    if (text.trim()) merged.push(text);
  }
  const seen = new Set();
  const deduped = [];
  for (let index = merged.length - 1; index >= 0; index -= 1) {
    const text = merged[index];
    if (seen.has(text)) continue;
    seen.add(text);
    deduped.push(text);
  }
  deduped.reverse();
  return deduped.length > MAX_COMPOSER_HISTORY ? deduped.slice(-MAX_COMPOSER_HISTORY) : deduped;
}

export function shouldNavigateInputHistory({
  key,
  code,
  value = '',
  editedSinceHistory = false,
  historyLength = 0,
  historyPointer = -1,
  hasNonTextContent = false,
  altKey = false,
  ctrlKey = false,
  metaKey = false,
  shiftKey = false,
} = {}) {
  const event = { key, code, altKey, ctrlKey, metaKey, shiftKey };
  if (!isInputHistoryNavigationMode({ value, editedSinceHistory, hasNonTextContent, historyPointer })) return false;
  if (isShortcut(event, 'historyPrevious')) return historyLength > 0;
  if (isShortcut(event, 'historyNext')) return historyPointer >= 0;
  return false;
}

export function getNextInputHistoryPointer({ key, code, altKey, ctrlKey, metaKey, shiftKey, historyLength = 0, historyPointer = -1 } = {}) {
  const count = Math.max(0, Math.trunc(Number(historyLength) || 0));
  const pointer = Number.isInteger(historyPointer) ? historyPointer : -1;

  if (isShortcut({ key, code, altKey, ctrlKey, metaKey, shiftKey }, 'historyPrevious')) {
    if (count === 0) return -1;
    return pointer === -1 ? count - 1 : Math.max(0, pointer - 1);
  }

  if (isShortcut({ key, code, altKey, ctrlKey, metaKey, shiftKey }, 'historyNext')) {
    if (pointer < 0) return -1;
    const next = pointer + 1;
    return next >= count ? -1 : next;
  }

  return pointer;
}
