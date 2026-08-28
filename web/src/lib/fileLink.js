// 判定 markdown 链接的语义:外链 / 页内锚点 / 本地文件 / 会话 / 应拒绝。
//
// 单一事实源:markdown.js 的 validateLink + link_open,与 Message.jsx 的点击拦截
// 都调这里。让「一个链接是否被渲染成可点文件链接」与「点击后是否走详情页预览」
// 用同一套判据 —— 否则很容易出现「渲染成蓝链但点了没反应」或「点了跳到错误 URL」。
// thread:// 会话链接同样走这里,避免被当成危险 scheme 剥成纯文本。
//
// 安全边界:validateLink 原本靠白名单挡 javascript:/data:。这里放行相对本地路径的
// 同时,任何带 URL scheme(且非 http/https/mailto/thread)的 href 一律判 reject,保留原有
// 防护。POSIX 绝对路径(/home/...)与 Windows 盘符绝对路径(N:\ 或 N:/)都算文件。

// 只认「结尾紧跟数字」的冒号作为 :行号(:12 或 :12:34,后者取首个数字为行)。
// 锚定到末尾 + 要求纯数字,避免误伤 Windows 盘符(N:\)或 URL scheme(http:)。
export function splitLineSuffix(raw) {
  const s = String(raw == null ? '' : raw);
  const m = /:(\d+)(?::\d+)?$/.exec(s);
  if (!m) return { path: s, line: null };
  return { path: s.slice(0, m.index), line: Number(m[1]) };
}

const WIN_ABS = /^[A-Za-z]:[\\/]/;         // N:\Users\... 或 N:/Users/...
const URL_SCHEME = /^[a-z][a-z0-9+.-]*:/i; // http: mailto: javascript: data: tel: ...

export function hasDirectoryMarker(value) {
  return /[/\\]$/.test(String(value == null ? '' : value).trim());
}

// 去掉路径尾部分隔符,但保留盘符根(N:\ / C:/)。定位文件树时要用目录本身,
// 不能把 `src/headless/` 原样塞进 tree key。
export function stripTrailingSeparators(rawPath) {
  const path = String(rawPath == null ? '' : rawPath);
  const stripped = path.replace(/[/\\]+$/, '');
  if (!stripped) return path;
  if (/^[A-Za-z]:$/.test(stripped)) return path;
  return stripped;
}

function normalizeComparablePath(value) {
  return stripTrailingSeparators(value).replace(/\\/g, '/');
}

function labelMarksSameDirectory(path, label) {
  const text = String(label == null ? '' : label).trim();
  if (!hasDirectoryMarker(text)) return false;
  return normalizeComparablePath(text) === normalizeComparablePath(path);
}

function asLocalPath(path, line, options = {}) {
  const directory = hasDirectoryMarker(path) || labelMarksSameDirectory(path, options.label);
  return {
    kind: directory ? 'directory' : 'file',
    path: directory ? stripTrailingSeparators(path) : path,
    line: directory ? null : line,
  };
}

// 会话跳转链接:thread://<session-id>,可选 ?workspace= / ?no_workspace=1。
// session-id 与 daemon `--session-id` 字符集一致:[A-Za-z0-9-_]{1,64}。
export function parseThreadLink(rawHref) {
  const href = String(rawHref == null ? '' : rawHref).trim();
  const match = /^thread:\/\/([A-Za-z0-9_-]{1,64})(?=[/?#]|$)/i.exec(href);
  if (!match) return null;
  const sessionId = match[1];
  const queryStart = href.indexOf('?');
  const hashStart = href.indexOf('#');
  const query = queryStart < 0
    ? ''
    : href.slice(queryStart + 1, hashStart >= 0 && hashStart > queryStart ? hashStart : undefined);
  const params = new URLSearchParams(query);
  const noWorkspace = params.get('no_workspace') === '1'
    || params.get('no_workspace') === 'true'
    || params.get('noWorkspace') === '1'
    || params.get('noWorkspace') === 'true';
  const workspaceHash = noWorkspace
    ? ''
    : String(params.get('workspace') || params.get('workspace_hash') || params.get('workspaceHash') || '').trim();
  return { sessionId, workspaceHash, noWorkspace };
}

export function threadSessionTargetFromClickEvent(event) {
  if (!event || event.defaultPrevented) return null;
  if (event.button != null && event.button !== 0) return null;
  if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return null;
  const target = event.target;
  if (!target || typeof target.closest !== 'function') return null;
  const anchor = target.closest('a[data-session-id]');
  if (!anchor) return null;
  const sessionId = String(anchor.getAttribute('data-session-id') || '').trim();
  if (!sessionId) return null;
  const noWorkspace = anchor.getAttribute('data-session-no-workspace') === 'true';
  const workspaceHash = noWorkspace
    ? ''
    : String(anchor.getAttribute('data-session-workspace') || '').trim();
  return { sessionId, workspaceHash, noWorkspace };
}

// 返回 { kind, path, line, sessionId?, workspaceHash?, noWorkspace? }。
// kind ∈ 'external' | 'anchor' | 'file' | 'directory' | 'session' | 'reject'。
// path/line 仅在 kind==='file' / 'directory' 有意义。
// sessionId 仅在 kind==='session' 有意义。
// 目录判定:href 或可见链接文案以 / 或 \ 结尾(模型/AGENT.md 的目录型链接约定)。
// markdown-it 在 parse 阶段就对 href 做百分号编码(mdurl.encode):中文文件名变成
// %E9%9A%8F%E6%9C%BA...,Windows 路径的反斜杠变成 %5C。这对 http 外链是必要的,
// 对本地路径却是两个真实故障:
//   1) data-file-path 带着编码值传给文件预览,拼 API URL 时再编码一次,服务端
//      收到 %E9%9A%8F 字面量找不到文件 —— 任何非 ASCII 文件名的链接必定点不开;
//   2) 反斜杠变 %5C 后 `N:\Users\x.md` 漏过盘符判定,`N:` 被当成 URL scheme
//      直接 reject,链接被剥成纯文本。
// 因此判定前先还原。放在 classifyFileLink 内部而不是各调用点,是为了让
// validateLink 与 link_open 继续共用同一套判据(见文件头的单一事实源说明);
// 顺带把 `javascript%3Aalert(1)` 这种编码绕过也还原成可识别的危险 scheme。
function decodePercentEncoding(value) {
  if (value.indexOf('%') < 0) return value;
  try {
    return decodeURIComponent(value);
  } catch {
    return value; // 非法 % 序列保持原样,后续判据照常处理
  }
}

export function classifyFileLink(rawHref, options = {}) {
  const href = decodePercentEncoding(String(rawHref == null ? '' : rawHref).trim());
  if (!href) return { kind: 'reject', path: '', line: null };
  // 页内锚点保持原样(当前页滚动)。
  if (href[0] === '#') return { kind: 'anchor', path: '', line: null };
  // 协议相对 URL(//host/path)当外链,避免被当成 POSIX 绝对路径。
  if (href.startsWith('//')) return { kind: 'external', path: '', line: null };
  // 真外链:新标签页打开(交给 link_open 加 target=_blank)。
  if (/^https?:/i.test(href)) return { kind: 'external', path: '', line: null };
  if (/^mailto:/i.test(href)) return { kind: 'external', path: '', line: null };
  // 会话链接必须赶在通用 scheme 拒绝之前,否则 thread:// 会被当成危险协议剥成纯文本。
  const thread = parseThreadLink(href);
  if (thread) {
    return {
      kind: 'session',
      path: thread.sessionId,
      line: null,
      sessionId: thread.sessionId,
      workspaceHash: thread.workspaceHash,
      noWorkspace: thread.noWorkspace,
    };
  }

  // Windows 盘符绝对路径:先判文件(盘符里的冒号不能当 scheme),再剥 :行号。
  if (WIN_ABS.test(href)) {
    const { path, line } = splitLineSuffix(href);
    return asLocalPath(path, line, options);
  }

  // 其余:先剥 :行号,再看剩下的是否带 scheme。带 scheme(javascript:/data:/...)一律拒绝。
  // 先剥行号是为了不把 `foo.md:42` 里的 `foo.md:` 误判成 scheme。
  const { path, line } = splitLineSuffix(href);
  if (URL_SCHEME.test(path)) return { kind: 'reject', path: '', line: null };
  // 无 scheme 的相对路径(docs/foo.md、./x、../y)或 POSIX 绝对路径(/home/...)→ 本地文件。
  return asLocalPath(path, line, options);
}
