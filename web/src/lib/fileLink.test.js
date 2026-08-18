import assert from 'node:assert/strict';
import {
  classifyFileLink,
  parseThreadLink,
  splitLineSuffix,
  stripTrailingSeparators,
  threadSessionTargetFromClickEvent,
} from './fileLink.js';
import { renderMarkdown } from './markdown.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:工作区相对路径(模型被 prompt 要求输出的主力形态)。
// 期望:判为 file,path 原样,无行号。这是「点击开详情页预览」的核心通路。
await run('relative workspace path classifies as file', () => {
  const r = classifyFileLink('src/prompt/system_prompt.cpp');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'src/prompt/system_prompt.cpp');
  assert.equal(r.line, null);
});

// 场景:目录型链接(href 或可见文案以 / 结尾)。
// 期望:判 directory,剥掉尾部分隔符,不带行号 —— 点击走文件树定位而不是预览。
await run('trailing-slash href classifies as directory', () => {
  const r = classifyFileLink('src/headless/');
  assert.equal(r.kind, 'directory');
  assert.equal(r.path, 'src/headless');
  assert.equal(r.line, null);
});

await run('label trailing slash classifies a bare directory href as directory', () => {
  const r = classifyFileLink('src/worktree', { label: 'src/worktree/' });
  assert.equal(r.kind, 'directory');
  assert.equal(r.path, 'src/worktree');
});

await run('windows directory path classifies as directory', () => {
  const r = classifyFileLink('N:\\Users\\shao\\acecode\\src\\headless\\');
  assert.equal(r.kind, 'directory');
  assert.equal(r.path, 'N:\\Users\\shao\\acecode\\src\\headless');
});

await run('file path is not a directory just because the label mentions a folder', () => {
  const r = classifyFileLink('src/headless/headless_runner.cpp', { label: 'src/headless/' });
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'src/headless/headless_runner.cpp');
});

await run('stripTrailingSeparators keeps drive roots and drops ordinary slashes', () => {
  assert.equal(stripTrailingSeparators('src/headless/'), 'src/headless');
  assert.equal(stripTrailingSeparators('src\\worktree\\'), 'src\\worktree');
  assert.equal(stripTrailingSeparators('N:\\'), 'N:\\');
  assert.equal(stripTrailingSeparators('C:/'), 'C:/');
});

// 场景:相对路径带 :行号(claude-code 风格 foo.cpp:130)。
// 期望:剥出 line=130,path 不含行号 —— 行号不能混进要读文件的 path。
await run('relative path with line suffix splits path and line', () => {
  const r = classifyFileLink('src/prompt/system_prompt.cpp:130');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'src/prompt/system_prompt.cpp');
  assert.equal(r.line, 130);
});

// 场景:./ 和 ../ 开头的相对路径。
// 期望:同样判 file(旧 validateLink 白名单只放 `.` 开头,这里保持兼容)。
await run('dot-relative paths classify as file', () => {
  assert.equal(classifyFileLink('./a/b.js').kind, 'file');
  assert.equal(classifyFileLink('../a/b.js').kind, 'file');
});

// 场景:Windows 盘符绝对路径(用户选了「工作区内 + 绝对路径」,模型偶尔吐全路径)。
// 期望:盘符里的冒号不被当 URL scheme,判 file;正/反斜杠都认;结尾 :行号照剥。
await run('windows absolute path classifies as file (both slash styles)', () => {
  assert.equal(classifyFileLink('N:\\Users\\shao\\acecode\\src\\a.cpp').kind, 'file');
  assert.equal(classifyFileLink('N:/Users/shao/acecode/src/a.cpp').kind, 'file');
  const r = classifyFileLink('N:\\Users\\shao\\acecode\\src\\a.cpp:42');
  assert.equal(r.kind, 'file');
  assert.equal(r.path, 'N:\\Users\\shao\\acecode\\src\\a.cpp');
  assert.equal(r.line, 42);
});

// 场景:POSIX 绝对路径。
// 期望:判 file(旧白名单已放 `/` 开头,行为不回退)。
await run('posix absolute path classifies as file', () => {
  assert.equal(classifyFileLink('/home/user/x.py').kind, 'file');
});

// 场景:thread:// 会话链接。
// 期望:判 session,抽出 session-id;可选 workspace / no_workspace 查询串。
await run('thread href classifies as session', () => {
  const r = classifyFileLink('thread://20260818-041607-0315');
  assert.equal(r.kind, 'session');
  assert.equal(r.sessionId, '20260818-041607-0315');
  assert.equal(r.workspaceHash, '');
  assert.equal(r.noWorkspace, false);
});

await run('thread href keeps workspace query and no_workspace flag', () => {
  const withWorkspace = classifyFileLink('thread://ses-1?workspace=abc123');
  assert.equal(withWorkspace.kind, 'session');
  assert.equal(withWorkspace.sessionId, 'ses-1');
  assert.equal(withWorkspace.workspaceHash, 'abc123');
  assert.equal(withWorkspace.noWorkspace, false);

  const noWorkspace = parseThreadLink('thread://ses-2?no_workspace=1');
  assert.equal(noWorkspace.sessionId, 'ses-2');
  assert.equal(noWorkspace.noWorkspace, true);
  assert.equal(noWorkspace.workspaceHash, '');
});

await run('invalid thread hrefs stay rejected', () => {
  assert.equal(classifyFileLink('thread://').kind, 'reject');
  assert.equal(classifyFileLink('thread://has space').kind, 'reject');
  assert.equal(classifyFileLink('thread:ses-1').kind, 'reject');
  assert.equal(parseThreadLink('javascript:alert(1)'), null);
});

// 场景:真外链 http/https/mailto。
// 期望:判 external(交给 link_open 加 target=_blank,不当文件预览)。
await run('http/https/mailto classify as external', () => {
  assert.equal(classifyFileLink('https://example.com/a').kind, 'external');
  assert.equal(classifyFileLink('http://example.com').kind, 'external');
  assert.equal(classifyFileLink('mailto:a@b.com').kind, 'external');
});

// 场景:协议相对 URL //host/path。
// 期望:判 external,而不是被误当成 POSIX 绝对文件路径。
await run('protocol-relative url classifies as external', () => {
  assert.equal(classifyFileLink('//cdn.example.com/x.js').kind, 'external');
});

// 场景:页内锚点。
// 期望:判 anchor,保持当前页滚动语义,不拦截、不预览。
await run('hash anchor classifies as anchor', () => {
  assert.equal(classifyFileLink('#section').kind, 'anchor');
});

// 回归:XSS 向量 javascript:/data:/vbscript: 必须被拒。
// 这是 validateLink 存在的原因 —— 放行相对路径时绝不能顺带放行危险 scheme。
await run('dangerous schemes are rejected', () => {
  assert.equal(classifyFileLink('javascript:alert(1)').kind, 'reject');
  assert.equal(classifyFileLink('data:text/html,<script>').kind, 'reject');
  assert.equal(classifyFileLink('vbscript:msgbox').kind, 'reject');
});

// 边界:空串。
// 期望:reject,避免生成空 data-file-path 的死链接。
await run('empty href is rejected', () => {
  assert.equal(classifyFileLink('').kind, 'reject');
  assert.equal(classifyFileLink('   ').kind, 'reject');
});

// splitLineSuffix 单元:只认末尾纯数字冒号,盘符/中段冒号不动。
await run('splitLineSuffix only strips trailing numeric colon', () => {
  assert.deepEqual(splitLineSuffix('a/b.c:12'), { path: 'a/b.c', line: 12 });
  assert.deepEqual(splitLineSuffix('a/b.c:12:5'), { path: 'a/b.c', line: 12 });
  assert.deepEqual(splitLineSuffix('a/b.c'), { path: 'a/b.c', line: null });
  assert.deepEqual(splitLineSuffix('N:\\x\\y.c'), { path: 'N:\\x\\y.c', line: null });
});

// 集成:renderMarkdown 把相对路径链接渲染成带 data-file-path 的可点锚点。
// 回归 bug:旧 validateLink 白名单 /^(https?:|mailto:|\/|\.|#)/ 会把裸相对路径 `docs/foo.md`
// 判非法 → markdown-it 剥成纯文本(连 <a> 都没有),表现为「有文件名没链接」。
await run('markdown renders relative file link with data-file-path', () => {
  const html = renderMarkdown('see [design](docs/spec.md:10) here');
  assert.match(html, /<a[^>]*data-file-path="docs\/spec\.md"/);
  assert.match(html, /data-file-line="10"/);
  assert.match(html, /class="[^"]*ace-file-link/);
  assert.match(html, /data-file-kind="file"/);
  // 文件链接不应带 target=_blank(不走浏览器新标签页,走详情页预览)。
  assert.doesNotMatch(html, /<a[^>]*data-file-path[^>]*target="_blank"/);
});

await run('markdown renders thread links as session chips', () => {
  const html = renderMarkdown('see [Fix encoding causing large git diffs](thread://20260818-041607-0315) here');
  assert.match(html, /<a[^>]*href="thread:\/\/20260818-041607-0315"/);
  assert.match(html, /data-session-id="20260818-041607-0315"/);
  assert.match(html, /class="[^"]*ace-thread-link/);
  assert.match(html, /class="[^"]*ace-cmd-token/);
  assert.match(html, /NewSession\.svg/);
  assert.match(html, /Fix encoding causing large git diffs/);
  assert.doesNotMatch(html, /target="_blank"/);
  assert.doesNotMatch(html, /data-file-path/);
});

await run('markdown linkifies bare thread URLs', () => {
  const html = renderMarkdown('open thread://20260818-042716-8d6a now');
  assert.match(html, /<a[^>]*data-session-id="20260818-042716-8d6a"/);
  assert.match(html, /class="[^"]*ace-thread-link/);
});

await run('threadSessionTargetFromClickEvent reads chip data attributes', () => {
  const anchor = {
    getAttribute: (name) => ({
      'data-session-id': 'ses-1',
      'data-session-workspace': 'ws-1',
      'data-session-no-workspace': null,
    }[name]),
  };
  const event = {
    defaultPrevented: false,
    button: 0,
    target: { closest: (selector) => (selector === 'a[data-session-id]' ? anchor : null) },
  };
  assert.deepEqual(threadSessionTargetFromClickEvent(event), {
    sessionId: 'ses-1',
    workspaceHash: 'ws-1',
    noWorkspace: false,
  });
  assert.equal(threadSessionTargetFromClickEvent({
    ...event,
    metaKey: true,
  }), null);
});

await run('markdown renders directory links with data-file-kind=directory', () => {
  const byHref = renderMarkdown('see [headless](src/headless/) here');
  assert.match(byHref, /<a[^>]*data-file-path="src\/headless"/);
  assert.match(byHref, /data-file-kind="directory"/);
  assert.doesNotMatch(byHref, /data-file-line/);

  const byLabel = renderMarkdown('see [src/worktree/](src/worktree) here');
  assert.match(byLabel, /<a[^>]*data-file-path="src\/worktree"/);
  assert.match(byLabel, /data-file-kind="directory"/);
});

// 集成:外链仍旧 target=_blank rel=noreferrer,且不带 data-file-path。
await run('markdown keeps external links as new-tab, not file', () => {
  const html = renderMarkdown('[site](https://example.com)');
  assert.match(html, /target="_blank"/);
  assert.match(html, /rel="noreferrer"/);
  assert.doesNotMatch(html, /data-file-path/);
});

// 集成:javascript: 链接被 validateLink 拒 → markdown-it 不生成 <a>,退化为纯文本。
await run('markdown drops javascript: link to plain text', () => {
  const html = renderMarkdown('[x](javascript:alert(1))');
  assert.doesNotMatch(html, /<a[^>]*href="javascript:/i);
});

// 回归:linkify 的 fuzzy 域名识别会把 SKILL.MD / AGENTS.COM / README.md /
// file.rs 当成 http://host(md/com/rs 都是真实 TLD)。编码助手输出里这些是文件名。
await run('markdown does not linkify filename-like host.tld tokens', () => {
  const html = renderMarkdown('See SKILL.MD and AGENTS.COM and README.md and file.rs');
  assert.doesNotMatch(html, /<a\b/);
  assert.match(html, /SKILL\.MD/);
  assert.match(html, /AGENTS\.COM/);
  assert.match(html, /README\.md/);
  assert.match(html, /file\.rs/);
});

// 显式 scheme / 邮箱仍应自动成链;显式 markdown 链接也不受 fuzzy 关闭影响。
await run('markdown still linkifies explicit URLs, emails, and [text](url)', () => {
  const url = renderMarkdown('Visit https://example.com please');
  assert.match(url, /href="https:\/\/example\.com"/);
  assert.match(url, /target="_blank"/);

  const mail = renderMarkdown('Email user@example.com please');
  assert.match(mail, /href="mailto:user@example\.com"/);

  const mdLink = renderMarkdown('see [skill](SKILL.MD) here');
  assert.match(mdLink, /<a[^>]*data-file-path="SKILL\.MD"/);
});

console.log('all fileLink tests passed');
