// 安全中心(Settings > 编码 > 安全中心,openspec add-security-center)的纯逻辑层。
// 后端契约:
//   GET/PUT /api/config/sandbox        { enabled, network_access, deny_defaults,
//                                        filesystem:{read[],write[],deny[]}, defaults:{deny[]}, platform:{...} }
//   GET/PUT /api/security/exec-rules   { dir, managed[], files:[{name, path, managed, scope, exists, error, rules[]}] }
//   GET     /api/security/audit        { entries[], has_more, total, next_before_id? }
//   GET     /api/security/audit/summary{ total, by_decision, by_category, last_ts_ms, blocked_paths[] }
// 组件只做渲染与请求,所有草稿归一 / 校验 / 序列化都在这里,Node 单测覆盖。

export const SECURITY_TABS = Object.freeze([
  { key: 'overview', label: '概览' },
  { key: 'files', label: '文件安全' },
  { key: 'commands', label: '命令安全' },
  { key: 'audit', label: '审计中心' },
]);

export const MANAGED_RULES_FILE = 'default.rules';
export const MANAGED_SANDBOXED_RULES_FILE = 'default.sandboxed.rules';

// ---------------------------------------------------------------------------
// 沙盒配置
// ---------------------------------------------------------------------------

function trimLine(value) {
  return String(value ?? '').replace(/[\r\n]+/g, '').trim();
}

// textarea 一行一条 → 去空白 / 去空行 / 去重(保序)。
export function parseEntryLines(text) {
  const seen = new Set();
  const out = [];
  for (const raw of String(text ?? '').split(/\r?\n/u)) {
    const entry = raw.trim();
    if (!entry || seen.has(entry)) continue;
    seen.add(entry);
    out.push(entry);
  }
  return out;
}

export function entryLinesText(entries) {
  return (Array.isArray(entries) ? entries : []).map((entry) => String(entry)).join('\n');
}

// 与后端 validate_sandbox_entry 同一套规则:记号 / 绝对路径通过;deny 允许通配。
// 返回错误文案,空串 = 合法。
export function validateSandboxEntry(rawEntry, allowGlob) {
  const entry = trimLine(rawEntry);
  if (!entry) return '条目不能为空';
  const hasGlob = /[*?[]/u.test(entry);
  if (hasGlob && !allowGlob) return `通配符只能用在黑名单里:${entry}`;
  if (entry.startsWith('~')) {
    if (entry.length > 1 && entry[1] !== '/' && entry[1] !== '\\') return `只支持 ~ 或 ~/路径,不支持 ~用户名:${entry}`;
    return '';
  }
  if (entry.startsWith(':')) {
    const token = entry.split(/[\\/]/u, 1)[0];
    if ([':workspace_roots', ':tmpdir', ':acecode_home'].includes(token)) return '';
    return `未知记号(可用 :workspace_roots / :tmpdir / :acecode_home):${entry}`;
  }
  if (hasGlob && (entry.startsWith('**/') || entry.startsWith('**\\'))) return '';
  const rooted = entry.startsWith('/') || entry.startsWith('\\') || /^[A-Za-z]:[\\/]/u.test(entry);
  if (!rooted) return `路径必须是绝对路径:${entry}`;
  return '';
}

export function sandboxDraft(snapshot) {
  return {
    enabled: snapshot?.enabled !== false,
    networkAccess: !!snapshot?.network_access,
    denyDefaults: snapshot?.deny_defaults !== false,
    read: entryLinesText(snapshot?.filesystem?.read),
    write: entryLinesText(snapshot?.filesystem?.write),
    deny: entryLinesText(snapshot?.filesystem?.deny),
  };
}

// 每张清单只报第一条错误,给 textarea 下面那一行提示。
export function validateSandboxDraft(draft) {
  const errors = {};
  const lists = [['read', false], ['write', false], ['deny', true]];
  for (const [key, allowGlob] of lists) {
    for (const entry of parseEntryLines(draft?.[key])) {
      const problem = validateSandboxEntry(entry, allowGlob);
      if (problem) { errors[key] = problem; break; }
    }
  }
  return errors;
}

export function sandboxPayload(draft) {
  return {
    enabled: !!draft?.enabled,
    network_access: !!draft?.networkAccess,
    deny_defaults: !!draft?.denyDefaults,
    filesystem: {
      read: parseEntryLines(draft?.read),
      write: parseEntryLines(draft?.write),
      deny: parseEntryLines(draft?.deny),
    },
  };
}

export function sandboxDraftHasChanges(draft, snapshot) {
  if (!draft || !snapshot) return false;
  return JSON.stringify(sandboxPayload(draft)) !== JSON.stringify(sandboxPayload(sandboxDraft(snapshot)));
}

// 追加一条到清单文本(已存在则原样返回)。
export function appendEntryLine(text, entry) {
  const clean = trimLine(entry);
  if (!clean) return text ?? '';
  const entries = parseEntryLines(text);
  if (entries.includes(clean)) return entryLinesText(entries);
  return entryLinesText([...entries, clean]);
}

// 被拦路径 → 建议加入可写清单的目录:最后一段像文件名(带扩展名)就取上一级,
// 否则就是它自己。与后端 suggested_write_root 不同,这里没有文件系统可查。
export function blockedPathToWritableRoot(path) {
  const clean = trimLine(path).replace(/[\\/]+$/u, '');
  if (!clean) return '';
  const parts = clean.split(/[\\/]/u);
  if (parts.length <= 1) return clean;
  const last = parts[parts.length - 1];
  if (/^[^.]+\.[^.]+$/u.test(last) || last.startsWith('.')) {
    const sep = clean.includes('\\') ? '\\' : '/';
    return parts.slice(0, -1).join(sep) || clean;
  }
  return clean;
}

// 平台探测 → 界面上要如实说明的限制。
export function platformNotes(platform) {
  const notes = [];
  if (!platform) return notes;
  if (!platform.available) {
    notes.push({ tone: 'danger', text: `沙箱后端不可用:${platform.reason || '未知原因'}` });
  }
  if (platform.read_isolation === false) {
    notes.push({ tone: 'warn', text: '当前平台的沙箱只能拦截写入,可读白名单与黑名单在这里不拦截读取(读隔离仅 macOS / Linux)。' });
  }
  if (platform.available && !platform.network_enforced) {
    notes.push({
      tone: 'warn',
      text: platform.network_best_effort
        ? '当前平台不能真正断网:关闭网络访问时只注入离线环境(代理指向无效端口、ssh 桩),不走代理的程序仍能联网。'
        : '当前后端不隔离网络。',
    });
  }
  return notes;
}

export function sandboxStatusLabel(snapshot) {
  if (!snapshot) return { tone: 'mute', text: '未加载' };
  if (!snapshot.enabled) return { tone: 'mute', text: '已关闭' };
  if (!snapshot.platform?.available) return { tone: 'danger', text: '不可用' };
  return { tone: 'ok', text: `已开启 · ${snapshot.platform.backend || ''}`.trim() };
}

// ---------------------------------------------------------------------------
// 命令规则
// ---------------------------------------------------------------------------

export const EXEC_RULE_DECISIONS = Object.freeze([
  { key: 'allow', label: '放行(沙箱外)', file: MANAGED_RULES_FILE, decision: 'allow' },
  { key: 'allow_sandboxed', label: '放行(沙箱内)', file: MANAGED_SANDBOXED_RULES_FILE, decision: 'allow' },
  { key: 'prompt', label: '询问', file: MANAGED_RULES_FILE, decision: 'prompt' },
  { key: 'forbidden', label: '禁止', file: MANAGED_RULES_FILE, decision: 'forbidden' },
]);

// 前端的即时反馈:与后端 is_banned_prefix 同源的核心子集(解释器 / shell / rm /
// sudo)。后端仍会完整校验,这里漏掉的会以 400 文案回来。
const BANNED_ALLOW_HEADS = new Set([
  'bash', 'sh', 'zsh', 'fish', 'dash', 'ksh', 'cmd', 'cmd.exe', 'powershell', 'powershell.exe', 'pwsh', 'pwsh.exe',
  'python', 'python3', 'python.exe', 'node', 'node.exe', 'ruby', 'perl', 'php', 'deno', 'bun',
  'rm', 'rm.exe', 'del', 'erase', 'rmdir', 'rd', 'sudo', 'doas', 'su', 'runas', 'eval', 'exec', 'source', 'xargs', 'env',
]);

let rowSequence = 0;
export function newExecRuleRow(overrides = {}) {
  rowSequence += 1;
  return { key: `rule-${Date.now()}-${rowSequence}`, decision: 'prompt', pattern: '', justification: '', ...overrides };
}

// `git status|diff --short` → [["git"], ["status", "diff"], ["--short"]]
export function parsePatternText(text) {
  return String(text ?? '')
    .trim()
    .split(/\s+/u)
    .filter(Boolean)
    .map((token) => token.split('|').map((alt) => alt.trim()).filter(Boolean))
    .filter((alternatives) => alternatives.length > 0);
}

export function patternDisplay(pattern) {
  if (!Array.isArray(pattern)) return '';
  return pattern.map((position) => (Array.isArray(position) ? position.join('|') : String(position))).join(' ');
}

function decisionKeyFor(fileName, decision) {
  if (fileName === MANAGED_SANDBOXED_RULES_FILE) return 'allow_sandboxed';
  return decision === 'prompt' ? 'prompt' : decision === 'forbidden' ? 'forbidden' : 'allow';
}

// 托管文件里的规则 → 可编辑行;非托管文件不进表格(只读展示)。
export function execRuleRows(snapshot) {
  const rows = [];
  for (const file of snapshot?.files || []) {
    if (!file?.managed) continue;
    for (const rule of file.rules || []) {
      rows.push(newExecRuleRow({
        decision: decisionKeyFor(file.name, rule.decision),
        pattern: rule.display || patternDisplay(rule.pattern),
        justification: rule.justification || '',
      }));
    }
  }
  return rows;
}

export function otherRuleFiles(snapshot) {
  return (snapshot?.files || []).filter((file) => file && !file.managed);
}

export function validateExecRuleRow(row) {
  const pattern = parsePatternText(row?.pattern);
  if (!pattern.length) return '命令前缀不能为空';
  const spec = EXEC_RULE_DECISIONS.find((item) => item.key === row?.decision);
  if (!spec) return '请选择决策';
  if (spec.decision === 'allow') {
    for (const head of pattern[0]) {
      if (BANNED_ALLOW_HEADS.has(head.toLowerCase())) {
        return `${head} 这类前缀不能放行(解释器 / shell / rm / sudo 可以执行任意内容)`;
      }
    }
  }
  return '';
}

export function validateExecRuleRows(rows) {
  const errors = {};
  const seen = new Map();
  for (const row of rows || []) {
    const problem = validateExecRuleRow(row);
    if (problem) { errors[row.key] = problem; continue; }
    const signature = `${row.decision} ${patternDisplay(parsePatternText(row.pattern))}`;
    if (seen.has(signature)) {
      errors[row.key] = '与另一条规则重复';
      errors[seen.get(signature)] = '与另一条规则重复';
      continue;
    }
    seen.set(signature, row.key);
  }
  return errors;
}

// PUT body:两个托管文件都带上(删光了也要写空数组,否则删除不落盘)。
export function execRulesPayload(rows) {
  const files = { [MANAGED_RULES_FILE]: [], [MANAGED_SANDBOXED_RULES_FILE]: [] };
  for (const row of rows || []) {
    const spec = EXEC_RULE_DECISIONS.find((item) => item.key === row.decision);
    if (!spec) continue;
    const pattern = parsePatternText(row.pattern).map((alts) => (alts.length === 1 ? alts[0] : alts));
    if (!pattern.length) continue;
    const rule = { pattern, decision: spec.decision };
    const justification = trimLine(row.justification);
    if (justification) rule.justification = justification;
    files[spec.file].push(rule);
  }
  return { files };
}

export function execRulesHaveChanges(rows, snapshot) {
  if (!snapshot) return false;
  return JSON.stringify(execRulesPayload(rows)) !== JSON.stringify(execRulesPayload(execRuleRows(snapshot)));
}

// ---------------------------------------------------------------------------
// 审计
// ---------------------------------------------------------------------------

export const AUDIT_CATEGORY_OPTIONS = Object.freeze([
  { key: '', label: '全部类型' },
  { key: 'command', label: '命令安全' },
  { key: 'file', label: '文件安全' },
  { key: 'tool', label: '工具调用' },
  { key: 'sandbox', label: '沙箱拦截' },
  { key: 'rule', label: '规则与授权' },
]);

export const AUDIT_DECISION_OPTIONS = Object.freeze([
  { key: '', label: '全部结果' },
  { key: 'allow', label: '放行' },
  { key: 'allow_session', label: '本次会话放行' },
  { key: 'allow_scoped', label: '放行目录' },
  { key: 'allow_remember', label: '放行并记住' },
  { key: 'deny', label: '拒绝' },
  { key: 'forbidden', label: '禁止' },
  { key: 'blocked', label: '沙箱拦截' },
]);

export const AUDIT_TIME_OPTIONS = Object.freeze([
  { key: 'all', label: '全部时间', ms: 0 },
  { key: '1h', label: '最近 1 小时', ms: 3600 * 1000 },
  { key: '24h', label: '最近 24 小时', ms: 24 * 3600 * 1000 },
  { key: '7d', label: '最近 7 天', ms: 7 * 24 * 3600 * 1000 },
  { key: '30d', label: '最近 30 天', ms: 30 * 24 * 3600 * 1000 },
]);

export const AUDIT_PAGE_SIZE = 50;

export function defaultAuditFilters() {
  return { category: '', decision: '', time: 'all', q: '' };
}

// 筛选 → query string(`?` 开头;没有条件时返回空串)。
export function auditQueryString(filters, { beforeId = 0, limit = AUDIT_PAGE_SIZE, now = Date.now() } = {}) {
  const params = new URLSearchParams();
  if (filters?.category) params.set('category', filters.category);
  if (filters?.decision) params.set('decision', filters.decision);
  const window = AUDIT_TIME_OPTIONS.find((item) => item.key === filters?.time);
  if (window?.ms > 0) params.set('since_ms', String(Math.max(0, now - window.ms)));
  const text = trimLine(filters?.q);
  if (text) params.set('q', text);
  if (limit > 0) params.set('limit', String(limit));
  if (beforeId > 0) params.set('before_id', String(beforeId));
  const query = params.toString();
  return query ? `?${query}` : '';
}

const SOURCE_LABELS = {
  auto: '自动判定',
  rule: '规则',
  session: '会话记忆',
  user: '用户确认',
  hook: '钩子',
  headless: '无头模式',
  goal: '无人值守',
  sandbox: '沙箱',
  none: '无确认通道',
};

const REASON_LABELS = {
  known_safe: '已知安全命令',
  unknown_command_sandboxed: '未知命令,沙箱内执行',
  unknown_command_without_sandbox: '未知命令且沙箱不可用',
  dangerous_command: '危险命令',
  rule_allow: '命中放行规则',
  rule_allow_sandboxed: '命中沙箱内放行规则',
  rule_prompt: '命中询问规则',
  rule_forbidden: '命中禁止规则',
  session_allow: '本次会话已放行',
  escalation_requested: '申请沙箱外执行',
  additional_permissions_requested: '申请额外权限',
  escalation_unattended: '无人值守下不能批越权',
  default_mode: '默认模式需确认',
  plan_mode: '计划模式',
  yolo: 'YOLO 模式',
  mode_auto: '自动模式',
  mode_default: '默认模式',
  mode_plan: '计划模式',
  mode_yolo: 'YOLO 模式',
  plan_file: '计划文件',
  unattended_goal: '无人值守自动放行',
  hook_allowed: '钩子放行',
  hook_denied: '钩子拒绝',
  headless_yolo: '无头 YOLO 放行',
  headless_no_channel: '无头模式无法确认',
  no_confirmation_channel: '没有确认通道',
  implicit: '非交互嵌入默认放行',
  confirmation: '需要确认',
  protected_rule: '内置保护规则',
  deny_rule_yolo: 'YOLO 下命中拒绝规则',
  exec_rules_protected: '规则文件只能由用户编辑',
  write_boundary: '超出写边界',
  path_validation: '路径校验失败',
  safe_edit_guard: '安全编辑守卫',
  permission_denied: '权限被拒',
  operation_not_permitted: '操作不被允许',
  read_only_file_system: '只读文件系统',
  policy_denied: '策略拒绝',
  failed_to_write_file: '写文件失败',
  access_denied: '访问被拒',
  sigsys: '系统调用被拦截',
  remember_rule: '写入规则文件',
  remember_rule_failed: '写入规则文件失败',
  scoped_grant: '放行被拒目录',
  session_grant: '本次会话授权额外权限',
};

function decisionTone(decision) {
  if (decision === 'deny' || decision === 'forbidden' || decision === 'blocked') return 'danger';
  if (decision === 'allow') return 'ok';
  return 'warn';
}

function actionVerb(entry) {
  const { category, decision } = entry;
  if (category === 'sandbox') return '沙箱拦截访问';
  if (category === 'rule') {
    if (entry.reason === 'scoped_grant') return '放行目录';
    if (entry.reason === 'session_grant') return '会话授权';
    return entry.reason === 'remember_rule_failed' ? '记住规则失败' : '记住命令规则';
  }
  const denied = decision === 'deny' || decision === 'forbidden';
  if (category === 'command') {
    if (denied) return decision === 'forbidden' ? '禁止执行命令' : '拒绝执行命令';
    return entry.sandbox && entry.sandbox !== 'full-access' ? '沙箱内执行命令' : '沙箱外执行命令';
  }
  if (category === 'file') return denied ? '拒绝修改文件' : '修改文件';
  return denied ? '拒绝调用工具' : '调用工具';
}

export function auditRowPresentation(entry) {
  const category = AUDIT_CATEGORY_OPTIONS.find((item) => item.key === entry?.category);
  const decision = AUDIT_DECISION_OPTIONS.find((item) => item.key === entry?.decision);
  const target = entry?.target || entry?.detail?.command || '';
  const meta = [
    SOURCE_LABELS[entry?.source] || entry?.source || '',
    REASON_LABELS[entry?.reason] || entry?.reason || '',
    entry?.sandbox ? `沙箱:${entry.sandbox}` : '',
    entry?.tool && entry.tool !== 'bash' ? entry.tool : '',
  ].filter(Boolean);
  const paths = Array.isArray(entry?.detail?.paths) ? entry.detail.paths : [];
  return {
    id: entry?.id,
    tsMs: entry?.ts_ms || 0,
    categoryLabel: category?.label || entry?.category || '',
    decisionLabel: decision?.label || entry?.decision || '',
    tone: decisionTone(entry?.decision),
    title: `[${category?.label || entry?.category || ''}] ${actionVerb(entry || {})}${target ? `:${target}` : ''}`,
    meta: meta.join(' · '),
    extraPaths: paths.length > 1 ? paths : [],
    sessionId: entry?.session_id || '',
    cwd: entry?.cwd || '',
    snippet: entry?.detail?.snippet || '',
  };
}

// 追加下一页,按 id 去重(刷新与加载更多交错时不会出现重复行)。
export function mergeAuditEntries(existing, incoming) {
  const seen = new Set((existing || []).map((entry) => entry.id));
  const merged = [...(existing || [])];
  for (const entry of incoming || []) {
    if (seen.has(entry.id)) continue;
    seen.add(entry.id);
    merged.push(entry);
  }
  return merged;
}

export function auditSummaryCounts(summary) {
  const by = summary?.by_decision || {};
  const allowed = ['allow', 'allow_session', 'allow_scoped', 'allow_remember']
    .reduce((sum, key) => sum + (Number(by[key]) || 0), 0);
  const denied = (Number(by.deny) || 0) + (Number(by.forbidden) || 0);
  const blocked = Number(by.blocked) || 0;
  return { total: Number(summary?.total) || 0, allowed, denied, blocked };
}

// 导出文件名跟着后端的 Content-Disposition 走,拿不到就自己拼一个。
export function auditExportFilename(format, now = new Date()) {
  const pad = (value) => String(value).padStart(2, '0');
  const stamp = `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}_${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
  return `acecode_audit_${stamp}.${format === 'csv' ? 'csv' : 'jsonl'}`;
}

// ---------------------------------------------------------------------------
// 错误
// ---------------------------------------------------------------------------

export function securityErrorCode(error, action) {
  const status = error?.status;
  let code = error?.code;
  if (status === 404 || status === 405) code = 'SECURITY_UNSUPPORTED';
  else if (status === 401 || status === 403) code = 'SECURITY_AUTH_REQUIRED';
  else if (status === 503 && code === 'AUDIT_UNAVAILABLE') code = 'AUDIT_UNAVAILABLE';
  else if (status >= 500 && (!code || code === 'UNAVAILABLE')) code = 'SECURITY_UNAVAILABLE';
  return { code, status, action, message: error?.message || '' };
}
