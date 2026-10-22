// Agent Browser 新标签页「最近」(lib/agentBrowserRecentFiles.js)的纯逻辑测试。
//
// 场景:
// 1. 从 transcript 的工具项收集改过的文件:同一文件取最后一次改动时间,新的在前;
//    多文件补丁(apply_patch)每个文件各一项;失败的工具调用与没有 hunks 的工具不算。
// 2. 同一文件的不同写法(反斜杠 / ./ 前缀 / 盘符大小写)合并成一项。
// 3. 类型标签按扩展名(文档 / 图片 / PDF / 演示文稿 / 电子表格 / 网页 / 文件)。
// 4. 时间标签:当天只显示时刻,其它日期显示月日,跨年带年份。
// 5. 签名只随内容变化,供 ChatView 稳定引用。
import assert from 'node:assert/strict';
import {
  collectAgentBrowserRecentFiles,
  recentFileKind,
  recentFileName,
  recentFileTimeLabel,
  recentFileTypeLabel,
  recentFilesSignature,
} from './agentBrowserRecentFiles.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function toolItem(ts, hunks, overrides = {}) {
  return {
    kind: 'tool',
    id: `t${ts}`,
    ts,
    tool: {
      success: true,
      startedAtMs: ts,
      summary: { object: hunks[0]?.file || '' },
      hunks,
      ...overrides,
    },
  };
}

await run('collects changed files newest first, one entry per file', () => {
  const items = [
    { kind: 'msg', role: 'user', id: 'u1', ts: 1 },
    toolItem(100, [{ file: 'docs/plan.md' }]),
    toolItem(200, [{ file: 'src/app.js' }]),
    toolItem(300, [{ file: 'docs/plan.md' }, { file: 'docs/plan.md' }]),
    toolItem(400, [{ file: 'a.png' }, { file: 'b.pdf' }]),
    toolItem(500, [{ file: 'broken.txt' }], { success: false }),
    { kind: 'tool', id: 'bash', ts: 600, tool: { success: true, hunks: [] } },
  ];
  const files = collectAgentBrowserRecentFiles(items);
  assert.deepEqual(files.map((f) => [f.path, f.ts]), [
    ['b.pdf', 400],
    ['a.png', 400],
    ['docs/plan.md', 300],
    ['src/app.js', 200],
  ], '同一时刻的多文件补丁按出现顺序倒排,失败的写入与无 hunks 的工具不算');
  assert.equal(files[2].name, 'plan.md');
  assert.equal(files[2].kind, 'document');
  assert.deepEqual(collectAgentBrowserRecentFiles(null), []);
  assert.equal(collectAgentBrowserRecentFiles(items, { limit: 2 }).length, 2);
});

await run('different spellings of the same path merge into one entry', () => {
  const files = collectAgentBrowserRecentFiles([
    toolItem(10, [{ file: 'C:\\Work\\Repo\\README.md' }]),
    toolItem(20, [{ file: 'c:/work/repo/README.md' }]),
    toolItem(30, [{ file: './src/x.js' }]),
    toolItem(40, [{ file: 'src/x.js' }]),
  ]);
  assert.deepEqual(files.map((f) => [f.path, f.ts]), [
    ['src/x.js', 40],
    ['c:/work/repo/README.md', 20],
  ]);
});

await run('history tool items use their turn start time instead of the load-time fallback', () => {
  // 回归:持久化的工具消息没有时间戳,历史工具项的 ts 是加载时刻;直接拿来显示,
  // 打开三天前的会话「最近」里每个文件都显示成刚刚改的。
  const loadedAt = 9_000_000;
  const history = (file) => ({ ...toolItem(loadedAt, [{ file }]), messageId: `m-${file}` });
  const files = collectAgentBrowserRecentFiles([
    { kind: 'msg', role: 'user', id: 'u1', ts: 1000 },
    history('first.md'),
    { kind: 'msg', role: 'user', id: 'u2', ts: 2000 },
    history('second.md'),
    history('third.md'),
    // 实时事件建的工具项(没有 messageId)用事件时间。
    toolItem(2500, [{ file: 'live.md' }]),
  ]);
  assert.deepEqual(files.map((f) => [f.path, f.ts]), [
    ['live.md', 2500],
    ['third.md', 2000],
    ['second.md', 2000],
    ['first.md', 1000],
  ], '同一回合内后改的排前面');
  const orphan = collectAgentBrowserRecentFiles([history('orphan.md')]);
  assert.equal(orphan[0].ts, 0, '历史页从回合中间开始、找不到用户消息时不编造时间');
});

await run('hunks without their own file fall back to the tool summary path', () => {
  const files = collectAgentBrowserRecentFiles([
    toolItem(10, [{ old_start: 1, lines: [] }], { summary: { object: 'notes/todo.txt' } }),
  ]);
  assert.deepEqual(files.map((f) => f.path), ['notes/todo.txt']);
});

await run('type labels follow the extension like Codex recents', () => {
  const cases = {
    'a/b/report.MD': '文档',
    'pic.webp': '图片',
    'paper.pdf': 'PDF',
    'deck.pptx': '演示文稿',
    'sheet.csv': '电子表格',
    'site/index.html': '网页',
    'main.cpp': '文件',
    Makefile: '文件',
    '.env': '文件',
  };
  for (const [path, label] of Object.entries(cases)) {
    assert.equal(recentFileTypeLabel(recentFileKind(path)), label, path);
  }
  assert.equal(recentFileName('C:\\dir\\file.txt'), 'file.txt');
});

await run('time label shows the clock today and month/day otherwise', () => {
  const now = new Date(2026, 9, 21, 18, 30);
  const calls = [];
  const format = (date, options) => {
    calls.push(options);
    return `${date.getMonth() + 1}/${date.getDate()} ${JSON.stringify(options)}`;
  };
  recentFileTimeLabel(new Date(2026, 9, 21, 12, 40).getTime(), now, format);
  recentFileTimeLabel(new Date(2026, 9, 20, 9, 0).getTime(), now, format);
  recentFileTimeLabel(new Date(2025, 11, 31, 9, 0).getTime(), now, format);
  assert.deepEqual(calls, [
    { hour: 'numeric', minute: '2-digit' },
    { month: 'short', day: 'numeric' },
    { month: 'short', day: 'numeric', year: 'numeric' },
  ]);
  assert.equal(recentFileTimeLabel(0, now, format), '', '没有时间戳时不显示');
});

await run('signature changes only with content', () => {
  const items = [toolItem(1, [{ file: 'a.md' }])];
  const a = collectAgentBrowserRecentFiles(items);
  const b = collectAgentBrowserRecentFiles([...items]);
  assert.notEqual(a, b);
  assert.equal(recentFilesSignature(a), recentFilesSignature(b));
  assert.notEqual(
    recentFilesSignature(a),
    recentFilesSignature(collectAgentBrowserRecentFiles([...items, toolItem(2, [{ file: 'a.md' }])])),
  );
});
