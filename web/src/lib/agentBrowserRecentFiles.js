// Agent Browser 新标签页「最近」:本会话产出 / 修改过的文件。
//
// 对照 Codex 桌面版(thread-side-panel-new-tab 的 Recents 区):它列当前对话的输出
// 产物(文档、图片、PDF、网站…),每行「图标 + 名称 + 类型 + 时间」,点开后用该产物
// 替换这个新标签页。ACECode 对应的数据是 transcript 里带 hunks 的工具调用(与「会话
// 变更」面板同一来源):每个文件取最后一次改动的时间,新的在前。
import { formatDateTime } from './format.js';

export const AGENT_BROWSER_RECENT_FILES_LIMIT = 50;

const KIND_BY_EXTENSION = new Map([
  ...['md', 'markdown', 'mdx', 'txt', 'rst', 'adoc', 'doc', 'docx', 'rtf', 'odt']
    .map((ext) => [ext, 'document']),
  ...['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'svg', 'ico', 'avif', 'tif', 'tiff']
    .map((ext) => [ext, 'image']),
  ['pdf', 'pdf'],
  ...['ppt', 'pptx', 'key', 'odp'].map((ext) => [ext, 'presentation']),
  ...['xls', 'xlsx', 'xlsm', 'csv', 'tsv', 'ods'].map((ext) => [ext, 'spreadsheet']),
  ...['html', 'htm'].map((ext) => [ext, 'website']),
]);

const KIND_LABELS = Object.freeze({
  document: '文档',
  image: '图片',
  pdf: 'PDF',
  presentation: '演示文稿',
  spreadsheet: '电子表格',
  website: '网页',
  other: '文件',
});

function text(value) {
  return typeof value === 'string' ? value.trim() : '';
}

export function recentFileName(path) {
  const parts = text(path).split(/[\\/]/).filter(Boolean);
  return parts.length ? parts[parts.length - 1] : text(path);
}

export function recentFileKind(path) {
  const name = recentFileName(path);
  const dot = name.lastIndexOf('.');
  if (dot <= 0) return 'other';
  return KIND_BY_EXTENSION.get(name.slice(dot + 1).toLowerCase()) || 'other';
}

export function recentFileTypeLabel(kind) {
  return KIND_LABELS[kind] || KIND_LABELS.other;
}

// 同一文件的不同写法(反斜杠 / ./ 前缀 / Windows 盘符大小写)视为同一项。
function pathKey(path) {
  let key = text(path)
    .replace(/\\/g, '/')
    .replace(/^\/\/\?\//, '')
    .replace(/^\.\/+/, '')
    .replace(/\/{2,}/g, '/');
  if (/^[A-Za-z]:\//.test(key)) key = key.toLowerCase();
  return key;
}

function positiveNumber(value) {
  const n = Number(value || 0);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

// 工具项改动文件的时间。持久化的工具消息不带时间戳,sessionTranscript 给历史工具项
// 填的是加载时刻(兜底),拿来显示会让所有旧改动都显示成「刚刚」;历史项(带 messageId
// 字段,实时事件建的工具项没有)改用所在回合用户消息的时间 —— 用户消息落盘时带时间戳。
function toolChangeTimestamp(item, turnStartTs) {
  const fromHistory = Object.prototype.hasOwnProperty.call(item, 'messageId');
  const ts = fromHistory ? 0 : positiveNumber(item.ts || item.tool?.startedAtMs);
  return Math.max(ts, turnStartTs);
}

function changedFilesOfTool(tool) {
  const files = [];
  const fallback = text(tool?.summary?.object);
  for (const hunk of Array.isArray(tool?.hunks) ? tool.hunks : []) {
    const file = text(hunk?.file) || fallback;
    if (file && !files.includes(file)) files.push(file);
  }
  return files;
}

// transcript items → [{ key, path, name, kind, ts }],按最后改动时间倒序。
// 失败的工具调用不算(文件没有真正写入)。
export function collectAgentBrowserRecentFiles(items, { limit = AGENT_BROWSER_RECENT_FILES_LIMIT } = {}) {
  if (!Array.isArray(items)) return [];
  const byKey = new Map();
  let order = 0;
  let turnStartTs = 0;
  for (const item of items) {
    if (item?.kind === 'msg' && item.role === 'user') {
      turnStartTs = positiveNumber(item.ts) || turnStartTs;
      continue;
    }
    if (item?.kind !== 'tool' || item.tool?.success === false) continue;
    const files = changedFilesOfTool(item.tool);
    if (files.length === 0) continue;
    const ts = toolChangeTimestamp(item, turnStartTs);
    for (const path of files) {
      const key = pathKey(path);
      if (!key) continue;
      order += 1;
      const previous = byKey.get(key);
      if (previous && previous.ts > ts) continue;
      byKey.set(key, {
        key,
        path,
        name: recentFileName(path),
        kind: recentFileKind(path),
        ts: ts || previous?.ts || 0,
        order,
      });
    }
  }
  return [...byKey.values()]
    .sort((a, b) => (b.ts - a.ts) || (b.order - a.order))
    .slice(0, Math.max(0, limit))
    .map(({ order: _order, ...file }) => file);
}

export function recentFilesSignature(files) {
  return (Array.isArray(files) ? files : [])
    .map((file) => `${file.key}\u0000${file.ts}`)
    .join('\u0001');
}

// 当天只显示时刻,其它日期显示月日(跨年再带年份),与 Codex 一致。
export function recentFileTimeLabel(ts, now = new Date(), format = formatDateTime) {
  const value = Number(ts);
  if (!Number.isFinite(value) || value <= 0) return '';
  const date = new Date(value);
  if (date.toDateString() === now.toDateString()) {
    return format(date, { hour: 'numeric', minute: '2-digit' });
  }
  return format(date, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() === now.getFullYear() ? {} : { year: 'numeric' }),
  });
}
