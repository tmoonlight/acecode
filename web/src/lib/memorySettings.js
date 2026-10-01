// 设置 > 个性化 > 记忆 的纯逻辑(openspec unify-memory-system)。
//
// daemon 侧接口(src/apps/web/routes/routes_memory.cpp):
//   GET/PUT /api/config/memory              使用记忆 / 记忆摘要设置;PUT 是 PATCH 语义,
//                                           summary 也可以只给一部分字段
//   GET     /api/memory?workspace=<hash>    全局与当前工作区的条目 + 记忆摘要状态
//   GET/PUT/DELETE /api/memory/<scope>/<name>?workspace=<hash>
//   POST    /api/memory/reset {scope, workspace}
//
// 组件(components/MemorySettings.jsx)只负责渲染与请求编排:载荷规整、PATCH 构造、
// 条目排序、作用域页签、状态整理、编辑表单校验与错误文案都在这里,有 Node 单测
// (memorySettings.test.js)。

export const MEMORY_SCOPE_GLOBAL = 'global';
export const MEMORY_SCOPE_WORKSPACE = 'workspace';
export const MEMORY_SCOPES = Object.freeze([MEMORY_SCOPE_GLOBAL, MEMORY_SCOPE_WORKSPACE]);

export const MEMORY_TYPES = Object.freeze(['user', 'feedback', 'project', 'reference']);
export const MEMORY_TYPE_OPTIONS = Object.freeze([
  Object.freeze({ value: 'user', label: '用户' }),
  Object.freeze({ value: 'feedback', label: '反馈' }),
  Object.freeze({ value: 'project', label: '项目' }),
  Object.freeze({ value: 'reference', label: '参考' }),
]);

// 与 daemon validate_memory_name 同一规则:[A-Za-z0-9_-]{1,64}。不合规的条目名拼进
// URL 只会得到 400,列表里直接丢掉,避免渲染出点不开的行。
const MEMORY_NAME_PATTERN = /^[A-Za-z0-9_-]{1,64}$/;

// 前端用来表示「没有工作区」的占位 hash,daemon 解析不了(会回 404 UNKNOWN_WORKSPACE)。
// 注意 __local__ 不在其中:daemon 的 resolve_workspace 把它解析成自己的工作目录
// (单 daemon / 网页模式下首页「当前项目」就是它),是合法的当前工作区。
const PLACEHOLDER_WORKSPACE_HASHES = new Set(['__no_workspace__']);

function isObject(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function text(value) {
  return typeof value === 'string' ? value : '';
}

function nonNegativeInteger(value) {
  const n = Number(value);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
}

// ─── 工作区与路径 ──────────────────────────────────────────────────────────

export function memoryWorkspaceHash(value) {
  const hash = typeof value === 'string' ? value.trim() : '';
  return hash && !PLACEHOLDER_WORKSPACE_HASHES.has(hash) ? hash : '';
}

function workspaceQuery(workspaceHash) {
  const hash = memoryWorkspaceHash(workspaceHash);
  return hash ? `?workspace=${encodeURIComponent(hash)}` : '';
}

export function memoryOverviewPath(workspaceHash = '') {
  return `/api/memory${workspaceQuery(workspaceHash)}`;
}

// 全局作用域的条目与工作区无关:不带 workspace 参数,这样当前工作区被移除 / 未登记
// (404 UNKNOWN_WORKSPACE)时全局记忆仍然可以查看、编辑和删除。
export function memoryEntryPath(scope, name, workspaceHash = '') {
  const hash = scope === MEMORY_SCOPE_WORKSPACE ? workspaceHash : '';
  return `/api/memory/${encodeURIComponent(String(scope || ''))}/${encodeURIComponent(String(name || ''))}`
    + workspaceQuery(hash);
}

export function memoryResetBody(scope, workspaceHash = '') {
  return {
    scope,
    workspace: scope === MEMORY_SCOPE_WORKSPACE ? memoryWorkspaceHash(workspaceHash) : '',
  };
}

// ─── 设置(/api/config/memory) ─────────────────────────────────────────────

export function normalizeMemorySettings(payload) {
  const source = isObject(payload) ? payload : {};
  const summary = isObject(source.summary) ? source.summary : {};
  return {
    enabled: source.enabled !== false,
    max_index_bytes: nonNegativeInteger(source.max_index_bytes),
    summary: {
      // 记忆摘要默认关闭:字段缺失 / 类型不对一律按关闭处理。
      enabled: summary.enabled === true,
      model_name: text(summary.model_name).trim(),
      idle_minutes: nonNegativeInteger(summary.idle_minutes),
      max_session_age_days: nonNegativeInteger(summary.max_session_age_days),
    },
    summary_available: source.summary_available === true,
  };
}

// 单个控件变化 → PUT body。只带变化的那个字段,其余设置由 daemon 保留现值,
// 两个窗口各改一项时不会互相覆盖。未知字段返回 null(不发请求)。
export function memorySettingsPatch(field, value) {
  switch (field) {
    case 'enabled':
      return { enabled: value === true };
    case 'summary.enabled':
      return { summary: { enabled: value === true } };
    case 'summary.model_name':
      return { summary: { model_name: text(value).trim() } };
    default:
      return null;
  }
}

// 乐观更新:把 PATCH 叠到当前设置上,得到请求返回之前先展示的值。
export function applyMemorySettingsPatch(settings, patch) {
  const base = normalizeMemorySettings(settings);
  if (!isObject(patch)) return base;
  return normalizeMemorySettings({
    ...base,
    ...(Object.prototype.hasOwnProperty.call(patch, 'enabled') ? { enabled: patch.enabled } : {}),
    summary: { ...base.summary, ...(isObject(patch.summary) ? patch.summary : {}) },
  });
}

// 控件可用性。记忆摘要的两个控件在「使用记忆」关闭或 daemon 不能运行摘要时禁用,
// 并给出原因(前者是用户自己的选择,灰字;后者是环境问题,警示色);读取中 / 保存中
// 整体禁用,避免在未知状态上切换。状态区跟随「实际生效」的记忆摘要:「使用记忆」
// 关闭时摘要不会运行,不再展示它的待整合 / 最近运行信息。
export function memorySettingsControls(settings, { loading = false, busy = false } = {}) {
  const ready = !!settings && !loading;
  let summaryHint = '';
  let summaryHintTone = '';
  if (ready && !settings.enabled) {
    summaryHint = '需要先开启「使用记忆」';
    summaryHintTone = 'mute';
  } else if (ready && !settings.summary_available) {
    summaryHint = '当前后台服务无法运行记忆摘要';
    summaryHintTone = 'warn';
  }
  return {
    enabledDisabled: !ready || busy,
    summaryDisabled: !ready || busy || !!summaryHint,
    summaryHint,
    summaryHintTone,
    showStatus: ready && settings.enabled && settings.summary.enabled === true,
  };
}

// GET /api/models → 可选的已保存模型名(去重、保序;以 "(" 开头的是 daemon 合成的
// 会话临时模型,不是可保存的选择)。
export function memorySummaryModelNames(payload) {
  const list = Array.isArray(payload)
    ? payload
    : (Array.isArray(payload?.models) ? payload.models : []);
  const names = [];
  for (const item of list) {
    const name = text(isObject(item) ? item.name : item).trim();
    if (!name || name.startsWith('(') || names.includes(name)) continue;
    names.push(name);
  }
  return names;
}

// 摘要模型下拉:「当前模型」(空值 = 沿用会话正在使用的模型)+ 已保存模型。已选的
// 模型不在列表里(被删除或改名)时追加一项标记不可用;模型列表还没读到时不下结论。
export function memorySummaryModelOptions(names, selected = '', { loaded = true } = {}) {
  const list = Array.isArray(names) ? names.filter((name) => typeof name === 'string' && name) : [];
  const current = text(selected).trim();
  const options = [
    { value: '', label: '当前模型', unavailable: false },
    ...list.map((name) => ({ value: name, label: name, unavailable: false })),
  ];
  if (current && !list.includes(current)) {
    options.push({ value: current, label: current, unavailable: loaded });
  }
  return options;
}

// ─── 条目与总览(/api/memory) ──────────────────────────────────────────────

export function memoryTimeMs(value) {
  if (typeof value === 'number') return Number.isFinite(value) && value > 0 ? value : 0;
  if (typeof value !== 'string' || !value.trim()) return 0;
  const ms = Date.parse(value);
  return Number.isFinite(ms) && ms > 0 ? ms : 0;
}

export function memoryEntryTimeMs(entry) {
  return memoryTimeMs(entry?.updated_at) || memoryTimeMs(entry?.created_at);
}

export function normalizeMemoryEntry(raw, scopeHint = '') {
  if (!isObject(raw)) return null;
  const name = text(raw.name).trim();
  if (!MEMORY_NAME_PATTERN.test(name)) return null;
  const scope = MEMORY_SCOPES.includes(raw.scope)
    ? raw.scope
    : (MEMORY_SCOPES.includes(scopeHint) ? scopeHint : MEMORY_SCOPE_GLOBAL);
  const sessions = Array.isArray(raw.source_sessions)
    ? raw.source_sessions.filter((id) => typeof id === 'string' && id.trim())
    : [];
  return {
    scope,
    name,
    description: text(raw.description),
    // 未知类型不硬套成四类之一:列表不画类型标记,编辑时不带 type,daemon 保留原值。
    type: MEMORY_TYPES.includes(raw.type) ? raw.type : '',
    created_at: text(raw.created_at),
    updated_at: text(raw.updated_at),
    source: raw.source === 'summary' ? 'summary' : 'manual',
    source_sessions: [...new Set(sessions)],
  };
}

// 最近更新的排最前;没有时间的排最后;同一时刻按名字排,结果稳定。
export function sortMemoryEntries(entries) {
  return [...(Array.isArray(entries) ? entries : [])].sort((left, right) => {
    const diff = memoryEntryTimeMs(right) - memoryEntryTimeMs(left);
    if (diff) return diff;
    if (left.name === right.name) return 0;
    return left.name < right.name ? -1 : 1;
  });
}

export function normalizeMemoryStatus(status) {
  const source = isObject(status) ? status : {};
  return {
    summary_enabled: source.summary_enabled === true,
    global_inbox: nonNegativeInteger(source.global_inbox),
    workspace_inbox: nonNegativeInteger(source.workspace_inbox),
    last_extraction_ms: memoryTimeMs(source.last_extraction_ms),
    last_consolidation_ms: memoryTimeMs(source.last_consolidation_ms),
    last_error: text(source.last_error).trim(),
    last_error_ms: memoryTimeMs(source.last_error_ms),
  };
}

function normalizeScope(raw, scope) {
  const source = isObject(raw) ? raw : {};
  const seen = new Set();
  const entries = [];
  for (const item of Array.isArray(source.entries) ? source.entries : []) {
    const entry = normalizeMemoryEntry(item, scope);
    if (!entry || seen.has(entry.name)) continue;
    seen.add(entry.name);
    entries.push({ ...entry, scope });
  }
  return {
    available: source.available === true,
    dir: text(source.dir),
    entries: sortMemoryEntries(entries),
  };
}

export function normalizeMemoryOverview(payload) {
  const source = isObject(payload) ? payload : {};
  const scopes = isObject(source.scopes) ? source.scopes : {};
  return {
    enabled: source.enabled !== false,
    scopes: {
      [MEMORY_SCOPE_GLOBAL]: normalizeScope(scopes[MEMORY_SCOPE_GLOBAL], MEMORY_SCOPE_GLOBAL),
      [MEMORY_SCOPE_WORKSPACE]: normalizeScope(scopes[MEMORY_SCOPE_WORKSPACE], MEMORY_SCOPE_WORKSPACE),
    },
    status: normalizeMemoryStatus(source.status),
  };
}

// 带了工作区却得到 404 UNKNOWN_WORKSPACE(工作区已移除 / 不是这个 daemon 登记的):
// 不带工作区再读一次,至少把全局记忆展示出来。路由不存在的裸 404 不在此列。
export function shouldRetryMemoryOverviewWithoutWorkspace(error, workspaceHash) {
  return !!memoryWorkspaceHash(workspaceHash) && memoryErrorCode(error) === 'UNKNOWN_WORKSPACE';
}

// 「全局 / 当前工作区」页签。工作区页签在没有可用工作区时禁用,hint 说明原因。
export function memoryScopeTabs(overview, { workspaceHash = '', workspaceUnknown = false } = {}) {
  const scopes = overview?.scopes || {};
  const workspace = scopes[MEMORY_SCOPE_WORKSPACE];
  let workspaceHint = '';
  if (!memoryWorkspaceHash(workspaceHash)) workspaceHint = '当前没有打开工作区';
  else if (workspaceUnknown) workspaceHint = '当前工作区未在后台服务中登记';
  else if (overview && !workspace?.available) workspaceHint = '当前工作区的记忆不可用';
  return [
    {
      scope: MEMORY_SCOPE_GLOBAL,
      label: '全局',
      count: scopes[MEMORY_SCOPE_GLOBAL]?.entries?.length || 0,
      available: true,
      hint: '',
    },
    {
      scope: MEMORY_SCOPE_WORKSPACE,
      label: '当前工作区',
      count: workspace?.entries?.length || 0,
      available: !workspaceHint && !!workspace?.available,
      hint: workspaceHint,
    },
  ];
}

// 用户选的页签不可用(工作区不可用 / 总览还没读到)时退回全局。
export function resolveMemoryScope(requested, tabs) {
  const tab = (Array.isArray(tabs) ? tabs : []).find((item) => item.scope === requested);
  return tab?.available ? tab.scope : MEMORY_SCOPE_GLOBAL;
}

// 记忆摘要状态:待整合观察数(工作区不可用时只报全局)、最近提炼 / 整合时间、最近错误。
// 错误早于之后的一次成功提炼 / 整合时标记为 stale —— 它已经恢复,只作记录展示。
export function memoryStatusView(status, { workspaceAvailable = false } = {}) {
  const value = normalizeMemoryStatus(status);
  const pending = [{ scope: MEMORY_SCOPE_GLOBAL, count: value.global_inbox }];
  if (workspaceAvailable) pending.push({ scope: MEMORY_SCOPE_WORKSPACE, count: value.workspace_inbox });
  const lastSuccessMs = Math.max(value.last_extraction_ms, value.last_consolidation_ms);
  return {
    pending,
    pendingTotal: pending.reduce((sum, item) => sum + item.count, 0),
    lastExtractionMs: value.last_extraction_ms,
    lastConsolidationMs: value.last_consolidation_ms,
    error: value.last_error
      ? {
        message: value.last_error,
        ms: value.last_error_ms,
        stale: value.last_error_ms > 0 && lastSuccessMs > value.last_error_ms,
      }
      : null,
  };
}

// ─── 编辑表单 ──────────────────────────────────────────────────────────────

export function normalizeMemoryEntryDetail(payload, scopeHint = '') {
  const entry = normalizeMemoryEntry(payload, scopeHint);
  if (!entry) return null;
  return {
    ...entry,
    body: text(payload.body),
    path: text(payload.path),
    redactions: nonNegativeInteger(payload.redactions),
  };
}

export function memoryEntryDraft(entry) {
  return {
    description: text(entry?.description),
    type: MEMORY_TYPES.includes(entry?.type) ? entry.type : '',
    body: text(entry?.body),
  };
}

// 描述写进 frontmatter,是单行文本:换行折成空格并去掉首尾空白。
function singleLine(value) {
  return text(value).replace(/\s*[\r\n]+\s*/g, ' ').trim();
}

// 与 daemon parse_memory_entry_edit 同口径:描述必填(全空白也算空),类型只能是四类之一。
export function validateMemoryEntryDraft(draft) {
  const errors = {};
  if (!singleLine(draft?.description)) errors.description = '描述不能为空';
  if (draft?.type && !MEMORY_TYPES.includes(draft.type)) errors.type = '类型无效';
  return errors;
}

export function memoryEntryDraftChanged(draft, entry) {
  const original = memoryEntryDraft(entry);
  return singleLine(draft?.description) !== singleLine(original.description)
    || text(draft?.type) !== original.type
    || text(draft?.body) !== original.body;
}

// PUT body:类型不明时不带 type,由 daemon 保留条目原来的类型。
export function memoryEntryUpdateBody(draft) {
  const body = {
    description: singleLine(draft?.description),
    body: text(draft?.body),
  };
  if (MEMORY_TYPES.includes(draft?.type)) body.type = draft.type;
  return body;
}

export function memoryTypeLabel(type) {
  return MEMORY_TYPE_OPTIONS.find((option) => option.value === type)?.label || '';
}

// ─── 错误 ──────────────────────────────────────────────────────────────────

const MEMORY_ERROR_TEXT = Object.freeze({
  MEMORY_UNSUPPORTED: '当前后台服务不支持记忆管理，请更新 ACECode 并重启后重试',
  MEMORY_AUTH_REQUIRED: '后台连接认证已失效，请重新连接 ACECode 后重试',
  MEMORY_UNAVAILABLE: '记忆服务在当前后台进程中不可用',
  UNAVAILABLE: '记忆服务在当前后台进程中不可用',
  UNKNOWN_WORKSPACE: '当前工作区未在后台服务中登记',
  NO_WORKSPACE: '当前没有打开工作区',
  NOT_FOUND: '这条记忆已不存在，请刷新列表',
  TIMEOUT: '请求超时,请重试',
  BAD_JSON: '请求格式错误',
});

// ApiError → 错误码。daemon 的结构化错误优先;没有错误码的裸 404 / 405 / 501 是
// 旧版 daemon 没有这组路由(/api/* 未匹配时直接 404,不回退到页面)。
export function memoryErrorCode(error) {
  const code = typeof error?.code === 'string' ? error.code : '';
  if (code) return code;
  const status = Number(error?.status) || 0;
  if (status === 404 || status === 405 || status === 501) return 'MEMORY_UNSUPPORTED';
  if (status === 401 || status === 403) return 'MEMORY_AUTH_REQUIRED';
  if (status >= 500) return 'MEMORY_UNAVAILABLE';
  return '';
}

function errorDetail(error) {
  const body = error?.body;
  const message = isObject(body) && typeof body.message === 'string' ? body.message.trim() : '';
  if (message) return message;
  // 非 HTTP 错误(网络断开等)没有 status,直接用异常文本。
  if (!(Number(error?.status) > 0) && typeof error?.message === 'string') return error.message.trim();
  return '';
}

// fallback 是这次操作的失败前缀(如「保存记忆失败」);已知错误码用固定文案,
// 其余(BAD_REQUEST / WRITE_FAILED / RESET_FAILED / PERSIST_FAILED…)拼上 daemon 的说明。
export function memoryErrorMessage(error, fallback = '') {
  const code = memoryErrorCode(error);
  if (code && Object.prototype.hasOwnProperty.call(MEMORY_ERROR_TEXT, code)) return MEMORY_ERROR_TEXT[code];
  const detail = errorDetail(error);
  if (fallback && detail) return `${fallback}:${detail}`;
  return fallback || detail || code || '未知错误';
}
