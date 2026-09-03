// 设置页「技能」tab 的纯数据整形:归一化 / 搜索过滤 / 项目-全局分组 / 启用计数。
// 数据源是 GET /api/skills(每条
// {name, description, enabled, source, status, error, error_code, path})。
// 组件层(SettingsPage.jsx SectionSkills)只做渲染,不承载这些逻辑。
//
// status 是后端对这份 SKILL.md 的判定:
//   ok      — 正常
//   warning — 能用,但元数据有问题(缺 description、frontmatter 未闭合……)
//   error   — 解析不出可用元数据,技能没装上
// 老 daemon 不发 status 字段,缺省按 ok 处理。

export const SKILL_STATUS_OK = 'ok';
export const SKILL_STATUS_WARNING = 'warning';
export const SKILL_STATUS_ERROR = 'error';

function normalizeStatus(value) {
  return value === SKILL_STATUS_ERROR || value === SKILL_STATUS_WARNING
    ? value
    : SKILL_STATUS_OK;
}

function text(value) {
  return typeof value === 'string' ? value : '';
}

// 归一化后端数组:丢掉无 name 的条目;source 只认 "project",其余(含
// 幽灵禁用条目的 "")一律归入 "global" 分组显示。
export function normalizeSkillList(data) {
  if (!Array.isArray(data)) return [];
  return data
    .map((item) => ({
      name: text(item?.name),
      description: text(item?.description),
      source: item?.source === 'project' ? 'project' : 'global',
      enabled: !!item?.enabled,
      status: normalizeStatus(item?.status),
      error: text(item?.error),
      errorCode: text(item?.error_code),
      path: text(item?.path),
    }))
    .filter((item) => item.name);
}

// 加载失败 = 磁盘上有 SKILL.md 但解析不出可用元数据。这类条目不能启停
// (没有可注册的技能名),UI 上以「加载失败」标出并给出原因。
export function skillLoadFailed(skill) {
  return skill?.status === SKILL_STATUS_ERROR;
}

// 有告警 = 技能能用,但元数据不完整,值得提示但不该拦住用户。
export function skillHasWarning(skill) {
  return skill?.status === SKILL_STATUS_WARNING;
}

// error_code → 本地化文案。后端的 `error` 字段是英文的(与 web 层其它错误
// 字符串一致,同时兼作日志),真正给用户看的措辞在这里,这样才进得了 i18n
// 词表。遇到不认识的 code(新 daemon + 旧前端)回落到后端原文。
const SKILL_ISSUE_MESSAGES = {
  unreadable: 'SKILL.md 为空或无法读取',
  missing_name: 'SKILL.md 缺少 name,目录名也无法作为技能名',
  unusable_name: '技能名不含可用于斜杠命令的字符',
  parse_error: '解析 SKILL.md 时出错',
  unterminated_frontmatter: 'frontmatter 以 --- 开头但没有闭合,整份文件被当作正文',
  missing_frontmatter: 'SKILL.md 缺少 --- 包裹的 YAML frontmatter',
  missing_description: 'frontmatter 缺少 description,模型无法判断何时使用该技能',
};

export function skillIssueMessage(skill) {
  if (!skill || skill.status === SKILL_STATUS_OK) return '';
  return SKILL_ISSUE_MESSAGES[skill.errorCode]
    || skill.error
    || '无法解析该技能的 SKILL.md';
}

// 列表里加载失败 / 有告警的条数,用于分组标题上的提示徽标。
export function skillIssueCounts(skills) {
  const list = Array.isArray(skills) ? skills : [];
  return {
    failed: list.filter(skillLoadFailed).length,
    warned: list.filter(skillHasWarning).length,
  };
}

// 实时搜索过滤:同时匹配技能名、description 与失败原因,大小写不敏感;
// 空查询返回原列表。失败条目的 description 为空,把 error 纳入匹配面才能
// 靠"未闭合"之类的关键词把出问题的技能捞出来。
export function filterSkills(skills, query) {
  const q = String(query || '').trim().toLowerCase();
  if (!q) return skills;
  return skills.filter((s) =>
    s.name.toLowerCase().includes(q) ||
    s.description.toLowerCase().includes(q) ||
    skillIssueMessage(s).toLowerCase().includes(q));
}

// 按来源分成「项目技能」「全局技能」两个列表,保持原有顺序。
export function groupSkillsBySource(skills) {
  const project = [];
  const global = [];
  for (const s of skills) {
    (s.source === 'project' ? project : global).push(s);
  }
  return { project, global };
}

// 右上角「N / M 已启用」摘要。基于完整列表(不受搜索过滤影响)。
//
// 分母只数能加载的技能:加载失败的条目永远启不起来,把它算进分母只会让
// 用户以为自己关掉了几个技能。失败数单独挂在 label 末尾。
export function skillsEnabledSummary(skills) {
  const list = Array.isArray(skills) ? skills : [];
  const loadable = list.filter((s) => !skillLoadFailed(s));
  const total = loadable.length;
  const enabled = loadable.reduce((n, s) => n + (s.enabled ? 1 : 0), 0);
  const failed = list.length - total;
  const label = failed
    ? `${enabled} / ${total} 已启用 · ${failed} 个加载失败`
    : `${enabled} / ${total} 已启用`;
  return { enabled, total, failed, label };
}

// 工作区折叠行右侧的紧凑计数,如 "1/2";有加载失败时追加 " ⚠N"。
export function enabledRatioLabel(skills) {
  const list = Array.isArray(skills) ? skills : [];
  const loadable = list.filter((s) => !skillLoadFailed(s));
  const enabled = loadable.reduce((n, s) => n + (s.enabled ? 1 : 0), 0);
  const failed = list.length - loadable.length;
  const base = `${enabled}/${loadable.length}`;
  return failed ? `${base} ⚠${failed}` : base;
}

// 归一化 GET /api/workspaces 的返回:丢掉无 hash/cwd 的条目。该端点只返回
// 已注册(desktop_visible)的工作区,无工作区会话的临时项天然不在其中。
export function normalizeWorkspaceList(data) {
  if (!Array.isArray(data)) return [];
  return data
    .map((w) => ({
      hash: typeof w?.hash === 'string' ? w.hash : '',
      cwd: typeof w?.cwd === 'string' ? w.cwd : '',
      name: typeof w?.name === 'string' && w.name ? w.name : (w?.cwd || ''),
    }))
    .filter((w) => w.hash && w.cwd);
}

// 搜索态下某工作区是否应自动展开:已加载且有命中。未加载(skills 为 null)
// 时不展开 — 展开与否由用户点击决定。
export function workspaceAutoExpand(skills, query) {
  if (!Array.isArray(skills)) return false;
  return filterSkills(skills, query).length > 0;
}
