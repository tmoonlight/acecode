// Web 路径选择器(openspec add-web-path-picker)的纯逻辑:路径显示形态、面包屑、
// 排序 / 可选性、确认目标推导、typeahead、起始目录。没有 DOM 与 React 依赖,Node 单测直跑。
//
// 路径形态与后端 fs_browser_handler 保持一致:正斜杠、Windows 盘符大写、盘根保留尾斜杠
// ("C:/" / "/")、其它路径不带尾斜杠。**不 canonical** —— junction 按用户浏览的写法保留。

export const PICKER_MODE_FOLDER = 'folder';
export const PICKER_MODE_FILE = 'file';
export const PATH_PICKER_PREFS_KEY = 'acecode.pathPicker.v1';
export const PATH_PICKER_PREFS_DEFAULTS = Object.freeze({ lastFolder: '' });

const DRIVE_ROOT_RE = /^[A-Za-z]:\/$/u;
const DRIVE_PREFIX_RE = /^[A-Za-z]:(?:\/|$)/u;

export function isWindowsDrivePath(text) {
  return DRIVE_PREFIX_RE.test(String(text || '').replace(/\\/gu, '/'));
}

// 用户输入 / 服务端返回的路径 → 显示形态。空串或相对路径返回 ''。
export function normalizePickerPath(text) {
  let s = String(text ?? '').trim().replace(/\\/gu, '/');
  if (!s) return '';
  const unc = s.startsWith('//');
  if (!unc && !s.startsWith('/') && !DRIVE_PREFIX_RE.test(s)) return '';
  if (DRIVE_PREFIX_RE.test(s)) s = s[0].toUpperCase() + s.slice(1);
  if (/^[A-Z]:$/u.test(s)) return `${s}/`;

  let prefix = '';
  let rest = s;
  if (unc) {
    prefix = '//';
    rest = s.slice(2);
  } else if (/^[A-Z]:\//u.test(s)) {
    prefix = s.slice(0, 3);
    rest = s.slice(3);
  } else {
    prefix = '/';
    rest = s.slice(1);
  }

  const segments = [];
  for (const part of rest.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') {
      if (segments.length > 0) segments.pop();
      continue;
    }
    segments.push(part);
  }
  if (segments.length === 0) return prefix === '//' ? '//' : prefix;
  return prefix + segments.join('/');
}

export function isPickerRoot(path) {
  const normalized = normalizePickerPath(path);
  return normalized === '/' || DRIVE_ROOT_RE.test(normalized);
}

// 父目录;根返回 ''(前端据此回到「此电脑」根节点视图)。
export function pickerParentPath(path) {
  const normalized = normalizePickerPath(path);
  if (!normalized || isPickerRoot(normalized)) return '';
  const slash = normalized.lastIndexOf('/');
  if (slash < 0) return '';
  const parent = normalized.slice(0, slash);
  if (/^[A-Z]:$/u.test(parent)) return `${parent}/`;
  if (parent === '') return '/';
  return normalizePickerPath(parent);
}

// 面包屑:[{ label, path }],根段的 label 是 "C:" 或 "/"。
export function pickerBreadcrumbs(path) {
  const normalized = normalizePickerPath(path);
  if (!normalized) return [];
  if (/^[A-Z]:\//u.test(normalized)) {
    const crumbs = [{ label: normalized.slice(0, 2), path: normalized.slice(0, 3) }];
    let current = normalized.slice(0, 3);
    for (const segment of normalized.slice(3).split('/').filter(Boolean)) {
      current = current.endsWith('/') ? `${current}${segment}` : `${current}/${segment}`;
      crumbs.push({ label: segment, path: current });
    }
    return crumbs;
  }
  const crumbs = [{ label: '/', path: '/' }];
  let current = '';
  for (const segment of normalized.split('/').filter(Boolean)) {
    current = `${current}/${segment}`;
    crumbs.push({ label: segment, path: current });
  }
  return crumbs;
}

export function joinPickerPath(dir, name) {
  const base = normalizePickerPath(dir);
  if (!base) return '';
  return base.endsWith('/') ? `${base}${name}` : `${base}/${name}`;
}

// 目录优先,再按名称做本地化数字感知排序(服务端已按 ASCII 不分大小写排过;这里用
// Intl.Collator 让 "file2" 排在 "file10" 前、中文按拼音)。
export function sortPickerEntries(entries, collator = defaultCollator()) {
  return [...(entries || [])].sort((a, b) => {
    const da = a?.kind === 'dir';
    const db = b?.kind === 'dir';
    if (da !== db) return da ? -1 : 1;
    return collator.compare(String(a?.name || ''), String(b?.name || ''));
  });
}

let cachedCollator = null;
function defaultCollator() {
  if (!cachedCollator) {
    cachedCollator = typeof Intl !== 'undefined' && Intl.Collator
      ? new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' })
      : { compare: (a, b) => (a < b ? -1 : a > b ? 1 : 0) };
  }
  return cachedCollator;
}

// 文件夹模式下文件不可选;文件模式下目录(用来进入)与文件都可选。
export function isSelectableEntry(entry, mode) {
  if (!entry) return false;
  if (mode === PICKER_MODE_FILE) return entry.kind === 'dir' || entry.kind === 'file';
  return entry.kind === 'dir';
}

export function selectableEntries(entries, mode) {
  return (entries || []).filter((entry) => isSelectableEntry(entry, mode));
}

// 主按钮要提交什么。返回 null 表示按钮不可用。
//   folder 模式:选中子目录 → 该目录;没选 → 当前目录;当前处于根节点视图 → null
//   file 模式:选中文件 → 该文件;否则 null
export function pickerSelectionTarget({ mode, currentPath, selected } = {}) {
  if (mode === PICKER_MODE_FILE) {
    if (selected?.kind === 'file' && selected.path) {
      return { path: selected.path, kind: 'file', label: 'open-file', name: selected.name || '' };
    }
    return null;
  }
  if (selected?.kind === 'dir' && selected.path) {
    return { path: selected.path, kind: 'dir', label: 'select-named', name: selected.name || '' };
  }
  const current = normalizePickerPath(currentPath);
  if (!current) return null;
  return { path: current, kind: 'dir', label: 'select-current', name: '' };
}

// 资源管理器式的字母跳转:按名称前缀匹配第一个可选条目;找不到再退化成子串匹配。
export function typeaheadMatch(entries, buffer) {
  const needle = String(buffer || '').toLowerCase();
  if (!needle) return null;
  const list = entries || [];
  const prefixHit = list.find((entry) => String(entry?.name || '').toLowerCase().startsWith(needle));
  if (prefixHit) return prefixHit;
  return list.find((entry) => String(entry?.name || '').toLowerCase().includes(needle)) || null;
}

// 弹窗起始目录:文件模式起始于调用方给的目录;文件夹模式优先调用方目录,其次上次确认的
// 目录,再其次主目录;都没有就回到根节点视图('')。
export function initialPickerPath({ mode, initialPath = '', lastFolder = '', home = '' } = {}) {
  const requested = normalizePickerPath(initialPath);
  if (requested) return requested;
  if (mode === PICKER_MODE_FILE) return normalizePickerPath(home);
  return normalizePickerPath(lastFolder) || normalizePickerPath(home);
}

// 记忆的目录:文件夹模式记确认的目录本身,文件模式记文件所在目录。
export function rememberedFolderAfterPick(result) {
  if (!result?.path) return '';
  if (result.kind === 'dir') return normalizePickerPath(result.path);
  return pickerParentPath(result.path);
}

export function validatePathPickerPrefs(value) {
  return !!value && typeof value === 'object' && typeof value.lastFolder === 'string';
}

// 盘符 / 根条目在列表里的显示名:卷标优先,没有卷标时由调用方给的默认文案兜底。
export function rootDisplayName(root, fallbackLabel) {
  const path = normalizePickerPath(root?.path);
  const label = String(root?.label || '').trim();
  if (/^[A-Z]:\/$/u.test(path)) {
    const letter = path.slice(0, 2);
    return label ? `${label} (${letter})` : `${fallbackLabel} (${letter})`;
  }
  if (label) return label;
  return path || fallbackLabel;
}

// 容量条百分比(0..100);缺容量信息返回 null。
export function rootUsagePercent(root) {
  const total = Number(root?.total_bytes);
  const free = Number(root?.free_bytes);
  if (!Number.isFinite(total) || total <= 0 || !Number.isFinite(free)) return null;
  return Math.max(0, Math.min(100, Math.round((1 - free / total) * 100)));
}

// HTTP 状态 → 弹窗内联提示的文案键。
export function browseErrorKey(status) {
  if (status === 403) return 'permission-denied';
  if (status === 404) return 'not-found';
  if (status === 400) return 'not-absolute';
  return 'io-error';
}

// 文件名按什么规则比较:Windows 与 macOS 默认卷不区分大小写,Linux 区分。
export function pickerNamesCaseInsensitive(os) {
  return os === 'windows' || os === 'macos';
}

// 「新建文件夹」弹窗输入框的长度上限(与 <input maxLength> 同口径,按 UTF-16 码元计)。
export const NEW_FOLDER_NAME_MAX_LENGTH = 50;

const WINDOWS_FORBIDDEN_NAME_CHARS = /[<>:"|?*]/u;
const WINDOWS_RESERVED_NAME_RE = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])$/iu;

// 新文件夹名在提交前的本地检查,与服务端 folder_name_problem 同一套规则(服务端仍是最终裁决)。
// 返回问题键,合法返回 ''。name 由调用方先 trim。
//   empty | too-long | dot | separator | control | windows-char | trailing | reserved | exists
export function newFolderNameProblem(name, { windows = false, entries = [], caseInsensitive = windows } = {}) {
  const text = String(name ?? '');
  if (!text) return 'empty';
  if (text.length > NEW_FOLDER_NAME_MAX_LENGTH) return 'too-long';
  if (text === '.' || text === '..') return 'dot';
  if (/[/\\]/u.test(text)) return 'separator';
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f\u007f]/u.test(text)) return 'control';
  if (windows) {
    if (WINDOWS_FORBIDDEN_NAME_CHARS.test(text)) return 'windows-char';
    if (/[. ]$/u.test(text)) return 'trailing';
    const base = text.split('.')[0].replace(/ +$/u, '');
    if (WINDOWS_RESERVED_NAME_RE.test(base)) return 'reserved';
  }
  const fold = (value) => (caseInsensitive ? value.toLowerCase() : value);
  const wanted = fold(text);
  if ((entries || []).some((entry) => fold(String(entry?.name || '')) === wanted)) return 'exists';
  return '';
}

// POST /api/fs/mkdir 的失败 → 弹窗内联提示的文案键。
export function mkdirErrorKey(status, body) {
  if (status === 409) return 'exists';
  if (status === 403) return 'permission-denied';
  if (status === 404) return 'parent-missing';
  if (status === 400 && body?.error === 'invalid name') return 'invalid-name';
  return 'io-error';
}
