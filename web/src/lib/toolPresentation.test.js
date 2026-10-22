import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { i18n } from '../i18n/index.js';
import { builtinTranscriptToolNames } from '../i18n/catalogs/toolTranscript.js';
import { canonicalTranscriptTool, presentTool } from './toolPresentation.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';
import { createTranscriptState, loadTranscriptHistory } from './sessionTranscript.js';

const t = lng => (key, options) => i18n.t(key, { ...options, lng });
const zh = t('zh-CN');
const en = t('en-US');
function run(name, fn) { fn(); console.log(`[pass] ${name}`); }
const root = fileURLToPath(new URL('../../../', import.meta.url));
function source(file) { return fs.readFileSync(path.join(root, file), 'utf8'); }
function cppFiles(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap(e => e.isDirectory()
    ? cppFiles(path.join(dir, e.name)) : e.name.endsWith('.cpp') ? [path.join(dir, e.name)] : []);
}

run('全部内置定义和动态工具家族都有独立中英文名称', () => {
  const names = new Set();
  const roots = ['src/adapters/tool', 'src/host/session_host/tools', 'src/host/loop'];
  for (const file of roots.flatMap(dir => cppFiles(path.join(root, dir)))) {
    const text = fs.readFileSync(file, 'utf8');
    for (const pattern of [/\b(?:def|definition)\.name\s*=\s*"([\w]+)"/g, /make_tool\(\s*"([\w]+)"/g, /\bkToolName\s*=\s*"([\w]+)"/g]) {
      for (const match of text.matchAll(pattern)) if (match[1] !== 'computer_') names.add(match[1].toLowerCase());
    }
  }
  for (const match of source('src/adapters/tool/computer_use_tool.cpp').matchAll(/\badd\("([\w]+)"/g)) names.add(`computer_${match[1]}`);
  for (const match of source('src/adapters/tool/agent_browser/browser_tools.cpp').matchAll(/^\s*"(browser_[\w]+)",?\s*$/gm)) {
    if (!match[1].endsWith('_failed')) names.add(match[1]);
  }
  // Runtime-selected names in create_message_tool are not string arguments.
  names.add('agent_send_message'); names.add('agent_followup_task');
  assert.ok(names.size >= 75, `inventory unexpectedly small: ${names.size}`);
  for (const name of names) {
    assert.ok(builtinTranscriptToolNames.includes(name), `missing native tool: ${name}`);
    const a = presentTool({ tool: name }, zh), b = presentTool({ tool: name }, en);
    assert.ok(a.builtin && b.builtin, name);
    assert.match(a.label, /[\u3400-\u9fff]/, name);
    assert.match(b.label, /^[A-Za-z]/, name);
    assert.doesNotMatch(b.label, /toolTranscript|_/, name);
  }
  console.log(`Verified ${names.size} registered native tool names`);
});

run('读取文件在各生命周期用同一名称，Windows/POSIX/UNC 路径仅显示文件名', () => {
  for (const file of ['N:\\work\\src\\index.vue', '/project/src/index.vue', '\\\\server\\share\\index.vue']) {
    for (const isDone of [true, false]) for (const success of [true, false]) {
      const entry = { tool: 'file_read', args: { file_path: file, max_bytes: 15000 }, isDone, success,
        summary: { verb: success ? 'Read' : 'File_read', object: file, metrics: [{ label: 'lines', value: '200' }] } };
      const a = presentTool(entry, zh), b = presentTool(entry, en);
      assert.equal(a.label + a.separator + a.detail, '读取文件：index.vue');
      assert.equal(b.label + b.separator + b.detail, 'Read File: index.vue');
      assert.equal(a.fields.filter(f => f.key === 'path').length, 1);
      assert.equal(a.fields.find(f => f.key === 'path').value, file);
      assert.equal(a.fields.find(f => f.key === 'max_bytes').value, '15000');
    }
  }
});

run('原始协议别名与无名称的历史摘要仍可读', () => {
  for (const tool of ['Read', 'read_file', 'functions.file_read']) assert.equal(canonicalTranscriptTool({ tool }), 'file_read');
  assert.equal(presentTool({ summary: { verb: 'Read', object: '/tmp/index.vue' } }, zh).detail, 'index.vue');
  assert.equal(presentTool({ tool: 'read', args: { file_path: '/tmp/index.vue' } }, en).label, 'Read File');
  assert.equal(presentTool({ tool: 'mcp__server__read', args: {} }, zh).label, 'mcp__server__read');
});

run('搜索仅显示搜索词，返回上限带标签且不重复搜索词', () => {
  const p = presentTool({ tool: 'grep', args: { pattern: '权限下发', path: 'web/src', include_pattern: '*.vue', head_limit: 50 },
    summary: { verb: 'Grep', object: '权限下发 · *.vue · 50' } }, zh);
  assert.equal(p.detail, '权限下发');
  assert.deepEqual(p.fields.map(f => f.value), ['web/src', '*.vue', '最多 50 条']);
  assert.equal(p.fields.some(f => f.value === '权限下发'), false);
});

run('命令字段不重复，耗时去重，原始输出与命令空白不被修改', () => {
  const entry = { tool: 'bash', isDone: true, args: { command: 'pnpm test', cwd: '/tmp', timeout_ms: 10000 },
    output: 'raw\n\nresult\n', elapsed: 3, summary: { verb: 'Ran', metrics: [{ label: 'time', value: '2.1s' }, { label: 'bytes', value: '5KB' }, { label: 'exit', value: '0' }] } };
  const original = structuredClone(entry);
  const p = presentTool(entry, zh);
  assert.equal(p.detail, 'pnpm test');
  assert.equal(p.fields.filter(f => f.key === 'elapsed').length, 1);
  assert.equal(p.fields.some(f => f.key === 'command'), false);
  assert.equal(p.output, entry.output);
  assert.deepEqual(entry, original);
});

run('详情保留参数/零值结果，省略空值默认开关并不暴露敏感参数', () => {
  const p = presentTool({ tool: 'browser_click', args: { target: '@e3', x: 0, y: 10, password: 'secret', enabled: false, text: '', key: '' } }, en);
  assert.equal(p.detail, '@e3');
  assert.equal(p.fields.find(f => f.key === 'x').value, '0');
  assert.equal(JSON.stringify(p).includes('secret'), false);
  assert.equal(p.fields.some(f => f.key === 'text'), false);
});

run('代码分析和模拟器操作按实际动作翻译，文件参数精简', () => {
  const p = presentTool({ tool: 'lsp', args: { operation: 'goToDefinition', file_path: '/src/main.cpp', line: 4, character: 8 } }, zh);
  assert.equal(p.label, '跳转定义'); assert.equal(p.detail, 'main.cpp');
  assert.equal(p.fields.some(f => f.key === 'operation'), false);
  assert.equal(presentTool({ tool: 'ios_simulator', args: { action: 'screenshot', device: 'iPhone' } }, en).label, 'Capture Simulator');
});

run('历史结构化非 Shell 工具恢复身份参数，投影不修改原始记录', () => {
  const args = { pattern: '权限', path: 'web', head_limit: 40 };
  const history = { messages: [
    { id: 'c', role: 'assistant', content: '', tool_calls: [{ id: 'one', type: 'function', function: { name: 'grep', arguments: JSON.stringify(args) } }] },
    { id: 'r', role: 'tool', tool_call_id: 'one', content: 'index.vue:42', metadata: { tool_summary: { verb: 'Grep', object: '权限 · web · 40' }, tool_success: true } },
  ] };
  const raw = loadTranscriptHistory(createTranscriptState(), history).state.items;
  const original = structuredClone(raw);
  const [item] = projectCollapsedTranscriptItems(raw, { messageAutoCollapse: false });
  assert.equal(item.tool.tool, 'grep'); assert.deepEqual(item.tool.args, args);
  assert.equal(presentTool(item.tool, zh).detail, '权限');
  assert.deepEqual(raw, original);
});

run('旧参数/结果包装只在结果区展示真实结果，实时参数优先', () => {
  const call = { kind: 'msg', id: 1, role: 'tool_call', toolCallId: 'one', content: '[Tool: file_read] {"file_path":"/tmp/index.vue"}' };
  const result = { kind: 'msg', id: 2, role: 'tool_result', toolCallId: 'one', content: '1: <template>' };
  const [item] = projectCollapsedTranscriptItems([call, result], { messageAutoCollapse: false });
  assert.equal(presentTool(item.tool, zh).output, '1: <template>');
  assert.match(item.tool.output, /工具调用/);
  const [live] = projectCollapsedTranscriptItems([call, { kind: 'tool', id: 3, tool: {
    tool: 'file_read', toolCallId: 'one', args: { file_path: '/tmp/current.vue' }, isDone: true,
  } }], { messageAutoCollapse: false });
  assert.equal(presentTool(live.tool, zh).detail, 'current.vue');
});

run('只含统计的结果仍有详情，长参数可在详情找回全文', () => {
  const p = presentTool({ tool: 'file_read', isDone: true, summary: { verb: 'Read', metrics: [{ label: 'cache', value: 'unchanged' }] } }, en);
  assert.equal(p.fields[0].value, 'Unchanged');
  const prompt = 'image '.repeat(40);
  const q = presentTool({ tool: 'image_generate', args: { prompt } }, zh);
  assert.ok(q.detail.length < prompt.length);
  assert.equal(q.fields.find(f => f.key === 'prompt').value, prompt.trim());
});

run('不会把用户输入当作枚举翻译，异常旧参数不导致渲染失败', () => {
  const p = presentTool({ tool: 'browser_fill', args: { target: '@e3', text: 'true', value: 'auto' } }, zh);
  assert.equal(p.fields.find(f => f.key === 'text').value, 'true');
  assert.equal(p.fields.find(f => f.key === 'value').value, 'auto');
  assert.equal(presentTool(null, zh).label, '调用工具');
  assert.equal(presentTool({ tool: 'vision_analyze', args: { image_paths: '/tmp/a.png' } }, zh).detail, 'a.png');
  assert.doesNotThrow(() => presentTool({ tool: 'apply_patch', hunks: {}, args: '{bad' }, zh));
  const thread = presentTool({ tool: 'read_thread', args: { threadId: 'thread-1', turnLimit: 5 } }, en);
  assert.equal(thread.detail, 'thread-1');
  assert.equal(thread.fields.length, 1);
});
