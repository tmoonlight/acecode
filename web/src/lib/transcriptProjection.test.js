import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { __test__, projectCollapsedTranscriptItems } from './transcriptProjection.js';
import { fallbackToolSummary } from './toolSummaryFallback.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function user(id, content = 'do work', ts = id * 1000) {
  return { kind: 'msg', id, role: 'user', content, ts, messageId: `u-${id}` };
}

function assistant(id, content, ts = id * 1000, extra = {}) {
  return { kind: 'msg', id, role: 'assistant', content, ts, ...extra };
}

function tool(id, {
  name = 'file_read',
  verb = 'Read',
  object = `file-${id}.txt`,
  isDone = true,
  success = true,
  ts = id * 1000,
  hunks = [],
  toolCallId = '',
} = {}) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone,
      success,
      tool: name,
      toolCallId,
      summary: { verb, object, metrics: [] },
      output: success ? '' : 'command failed\nstderr details',
      hunks,
    },
  };
}

function toolWrapper(id, role, content, extra = {}) {
  return {
    kind: 'msg',
    id,
    role,
    content,
    ts: id * 1000,
    ...extra,
  };
}

function compactNotice(id, operationId, stage, content, complete = false) {
  return {
    kind: 'msg',
    id,
    messageId: `compact-${id}`,
    role: 'system',
    content,
    ts: id * 1000,
    metadata: {
      transcript_only: true,
      compact_notice: true,
      compact_notice_id: operationId,
      compact_notice_stage: stage,
      compact_notice_complete: complete,
    },
  };
}

function taskComplete(id, summary = 'done', ts = id * 1000, success = true) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isTaskComplete: true,
      isDone: true,
      success,
      tool: 'task_complete',
      summary: { object: summary },
    },
  };
}

function completeSummaryTool(id, summary = 'done', ts = id * 1000) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone: true,
      success: true,
      tool: '',
      summary: {
        verb: 'complete',
        object: 'task',
        metrics: [{ label: 'summary', value: summary }],
      },
      output: '',
    },
  };
}

function askQuestionTool(id, ts = id * 1000) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone: true,
      success: true,
      tool: 'AskUserQuestion',
      toolCallId: 'call-ask',
      output: '',
      askUserQuestionResult: {
        items: [
          { question: '希望我直接修改还是先给出方案让你确认?', answer: '直接修改并补测试' },
          { question: '清理时机?', answer: 'onBeforeUnmount' },
        ],
      },
    },
  };
}

run('通用摘要按参数原序显示值并固定工具图标', () => {
  const summary = fallbackToolSummary('glob', '{"pattern":"**/*","path":"C:/repo","options":{"hidden":true}}');
  assert.deepEqual(summary, {
    verb: 'Glob',
    object: '**/* · C:/repo · {"hidden":true}',
    icon: '*',
    metrics: [],
  });
});

run('通用摘要折叠换行、掩码敏感值并安全处理畸形参数', () => {
  const redacted = fallbackToolSummary('mcp_echo', {
    text: 'first\n  second',
    api_key: 'private',
    nested: { db_password: 'also-private', value: 7 },
  });
  assert.equal(redacted.verb, 'Mcp_echo');
  assert.equal(redacted.object, 'first second · [REDACTED] · {"db_password":"[REDACTED]","value":7}');
  assert.equal(redacted.object.includes('private'), false);

  const malformed = fallbackToolSummary('glob', '{bad json');
  assert.equal(malformed.verb, 'Glob');
  assert.equal(malformed.object, '{bad json');
  assert.equal(malformed.icon, '*');
});

run('窄聊天栏保留完整单行工具名并只截断详情', () => {
  const activityLineSource = readFileSync(
    new URL('../components/ActivityLine.jsx', import.meta.url),
    'utf8',
  );
  const toolBlockSource = readFileSync(
    new URL('../components/ToolBlock.jsx', import.meta.url),
    'utf8',
  );

  assert.match(activityLineSource, /'whitespace-nowrap font-medium[^']*'/);
  assert.match(
    activityLineSource,
    /preserveLabel \? 'shrink-0' : 'min-w-0 max-w-\[62%\] truncate'/,
  );
  assert.match(activityLineSource, /min-w-0 truncate text-fg-mute/);
  assert.ok((toolBlockSource.match(/\bpreserveLabel\b/g) || []).length >= 3);
  assert.match(toolBlockSource, /const completedSummary = summary \|\| genericSummary;/);
});

run('普通工具展开文本统一使用带复制图标的小字号内容框', () => {
  const toolBlockSource = readFileSync(
    new URL('../components/ToolBlock.jsx', import.meta.url),
    'utf8',
  );
  const copyFrameSource = readFileSync(
    new URL('../components/CopyableCodeFrame.jsx', import.meta.url),
    'utf8',
  );
  const stylesSource = readFileSync(
    new URL('../styles/globals.css', import.meta.url),
    'utf8',
  );

  assert.match(toolBlockSource, /function ToolTextFrame\(/);
  assert.match(
    toolBlockSource,
    /ace-tool-output-frame overflow-hidden rounded-xl border border-border bg-surface/,
  );
  assert.match(toolBlockSource, /data-tool-output-frame="true"/);
  assert.ok((toolBlockSource.match(/<ToolTextFrame\b/g) || []).length >= 2);
  assert.ok((toolBlockSource.match(/fontSize: 'var\(--ace-font-size-code\)'/g) || []).length >= 1);
  assert.match(toolBlockSource, /text=\{fullToolOutput\}/);
  assert.match(toolBlockSource, /text=\{progressFrameText\}/);
  assert.match(copyFrameSource, /<VsIcon name="copy" size=\{14\}/);
  assert.match(copyFrameSource, /onClick=\{handleCopy\}/);
  assert.match(
    stylesSource,
    /\.ace-code-actions\s*\{[\s\S]*?position:\s*absolute;[\s\S]*?top:\s*6px;[\s\S]*?right:\s*6px;/,
  );
});

run('新建文件展开源码框，覆盖和编辑仍保留专用 diff', () => {
  const toolBlockSource = readFileSync(
    new URL('../components/ToolBlock.jsx', import.meta.url),
    'utf8',
  );
  const stylesSource = readFileSync(
    new URL('../styles/globals.css', import.meta.url),
    'utf8',
  );

  assert.match(
    toolBlockSource,
    /createdFile \? \(\s*<CreatedFileFrame source=\{createdFile\} \/>\s*\) : diffHtml \? \(/,
  );
  assert.match(toolBlockSource, /if \(createdFile \|\| !diffText\) return '';/);
  assert.match(toolBlockSource, /className="ace-diff ace-tool-diff"/);
  assert.doesNotMatch(toolBlockSource, /<ToolTextFrame text=\{diffText\}/);
  assert.match(stylesSource, /\.ace-tool-created-code\s*\{/);
  assert.match(stylesSource, /:root:not\(\[data-theme="dark"\]\) \.ace-tool-created-code \.hljs-keyword/);
});

run('Created 源码框随窄聊天容器收缩且只在 pre 内横向滚动', () => {
  const toolBlockSource = readFileSync(
    new URL('../components/ToolBlock.jsx', import.meta.url),
    'utf8',
  );
  const stylesSource = readFileSync(
    new URL('../styles/globals.css', import.meta.url),
    'utf8',
  );

  assert.match(toolBlockSource, /className="w-full min-w-0 max-w-\[88%\] pb-2 pt-1"/);
  assert.match(
    stylesSource,
    /\.ace-tool-created-code\s*\{[\s\S]*?min-width:\s*0;[\s\S]*?width:\s*100%;[\s\S]*?max-width:\s*100%;/,
  );
  assert.match(
    stylesSource,
    /\.ace-tool-created-code > pre\s*\{[\s\S]*?min-width:\s*0;[\s\S]*?width:\s*100%;[\s\S]*?max-width:\s*100%;[\s\S]*?overflow:\s*auto;/,
  );
});

run('完成的压缩通知投影为一个可展开 Context compacted 消息', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    compactNotice(2, 'operation-1', 'progress', 'Compacting conversation...'),
    compactNotice(3, 'operation-1', 'checkpoint', '--- [Compact Checkpoint] ---'),
    compactNotice(4, 'operation-1', 'summary', '[Conversation summary]\nlong summary'),
    compactNotice(5, 'operation-1', 'warning', 'Heads up', true),
  ], { deferTrailingToolSummary: true });

  assert.equal(projected.length, 2);
  const compacted = projected[1];
  assert.equal(compacted.kind, 'msg');
  assert.equal(compacted.role, 'system');
  assert.equal(compacted.metadata.compact_label, 'Context compacted');
  assert.equal(compacted.metadata.compact_notice_complete, true);
  assert.deepEqual(compacted.coveredItemIds, [2, 3, 4, 5]);
  assert.equal(
    compacted.content,
    'Compacting conversation...\n\n'
      + '--- [Compact Checkpoint] ---\n\n'
      + '[Conversation summary]\nlong summary\n\n'
      + 'Heads up',
  );
});

run('未完成或失败的压缩通知保持逐条可见', () => {
  const progress = compactNotice(
    2, 'operation-2', 'progress', 'Compacting conversation...', false,
  );
  const error = compactNotice(3, 'operation-2', 'error', 'provider unavailable', false);
  const projected = projectCollapsedTranscriptItems([
    user(1),
    progress,
    error,
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected, [user(1), progress, error]);
  assert.equal(projected[1].metadata.compact_notice_complete, false);
  assert.equal(projected[2].content, 'provider unavailable');
});

run('连续工具在 assistant 文本前折叠成一个 activity_summary', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Created', object: 'a.txt', name: 'file_write' }),
    tool(3, { verb: 'Edited', object: 'b.txt', name: 'file_edit' }),
    tool(4, { verb: 'Ran', object: 'pnpm test', name: 'bash' }),
    assistant(5, 'done'),
  ], { deferTrailingToolSummary: true });

  assert.equal(projected.length, 3);
  assert.equal(projected[1].kind, 'activity_summary');
  assert.equal(projected[1].mode, 'tools');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.match(projected[1].title, /已创建 1 个文件/);
  assert.match(projected[1].title, /已编辑 1 个文件/);
  assert.match(projected[1].title, /已运行 1 条命令/);
  assert.match(projected[1].title, /调用 1 个工具/);
  assert.equal(projected[2].content, 'done');
});

run('文件工具摘要不重复计入调用工具数量', () => {
  const title = __test__.summarizeToolItems([
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    tool(3, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    tool(4, { verb: 'Edited', object: 'c.md', name: 'file_edit' }),
  ]);

  assert.equal(title, '已编辑 1 个文件，读取 2 个文件');
  assert.doesNotMatch(title, /调用/);
});

run('非文件工具不会仅因 Read 摘要词被当成文件读取', () => {
  const title = __test__.summarizeToolItems([
    tool(2, { verb: 'Read', object: 'needle', name: 'grep' }),
  ]);

  assert.equal(title, '调用 1 个工具');
});

run('折叠活动摘要使用持久 ISO timestamp', () => {
  const toolTs = '2026-05-01T01:02:03Z';
  const projected = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, role: 'user', content: 'do work', timestamp: '2026-05-01T01:02:00Z' },
    {
      kind: 'tool',
      id: 2,
      timestamp: toolTs,
      tool: {
        isDone: true,
        success: true,
        tool: 'bash',
        summary: { verb: 'Ran', object: 'date', metrics: [] },
        output: '',
        hunks: [],
      },
    },
    { kind: 'msg', id: 3, role: 'assistant', content: 'done', timestamp: '2026-05-01T01:02:05Z' },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].ts, Date.parse(toolTs));
});

run('空 assistant 和工具调用返回行不会把连续工具拆成多个摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    assistant(2, ''),
    tool(3, { verb: 'Ran', object: 'cmd 1', name: 'bash' }),
    { kind: 'msg', id: 4, role: 'tool_result', content: 'ok', ts: 4000 },
    assistant(5, ''),
    tool(6, { verb: 'Ran', object: 'cmd 2', name: 'bash' }),
    { kind: 'msg', id: 7, role: 'tool_call', content: '[Tool: bash] cmd 3', ts: 7000 },
    tool(8, { verb: 'Ran', object: 'cmd 3', name: 'bash' }),
    assistant(9, 'all commands finished'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已运行 3 条命令，调用 3 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4, 5, 6, 7, 8]);
  assert.equal(projected[2].content, 'all commands finished');
});

run('流式尾部工具段未遇到文字时只投影一个稳定活动行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    tool(3, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    tool(4, { verb: 'Created', object: 'c.md', name: 'file_write' }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(projected[1].mode, 'live');
  assert.equal(projected[1].collapsedItems.length, 3);
  assert.equal(projected[1].collapsedItems[2].tool.summary.object, 'c.md');
});

run('流式尾部结构化工具隐藏冗余 tool_call 和 tool_result 包装行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_call', content: '[Tool: bash] {"command":"mkdir app"}', ts: 2000 },
    tool(3, { verb: 'Ran', object: 'mkdir app', name: 'bash' }),
    { kind: 'msg', id: 4, role: 'tool_result', content: '(no output)', ts: 4000 },
    { kind: 'msg', id: 5, role: 'tool_call', content: '[Tool: file_write] {"file_path":"app/index.html"}', ts: 5000 },
    tool(6, { verb: 'Created', object: 'app/index.html', name: 'file_write' }),
    { kind: 'msg', id: 7, role: 'tool_result', content: 'Created file: app/index.html', ts: 7000 },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.deepEqual(projected.map((item) => item.role).filter(Boolean), ['user']);
  assert.equal(projected[1].mode, 'live');
  assert.deepEqual(
    projected[1].collapsedItems.map((item) => item.tool.summary.object),
    ['mkdir app', 'app/index.html'],
  );
});

run('并行只读工具批量包装行不会误报旧记录缺少请求参数', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: file_read] {"file_path":"a.cpp"}'),
    toolWrapper(3, 'tool_call', '[Tool: file_read] {"file_path":"b.cpp"}'),
    toolWrapper(4, 'tool_call', '[Tool: grep] {"pattern":"needle"}'),
    tool(5, { verb: 'Read', object: 'a.cpp', name: 'file_read', toolCallId: 'call-a' }),
    tool(6, { verb: 'Read', object: 'b.cpp', name: 'file_read', toolCallId: 'call-b' }),
    tool(7, { verb: 'Read', object: 'grep results', name: 'grep', toolCallId: 'call-grep' }),
    toolWrapper(8, 'tool_result', 'A result'),
    toolWrapper(9, 'tool_result', 'B result'),
    toolWrapper(10, 'tool_result', 'grep result'),
    assistant(11, 'continue'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].collapsedItems.length, 3);
  assert.equal(projected[1].collapsedItems.some((item) => item.role === 'tool_result'), false);
  assert.equal(JSON.stringify(projected).includes('请求未记录'), false);
  assert.deepEqual(projected[1].collapsedItems.map((item) => item.tool.toolCallId), ['call-a', 'call-b', 'call-grep']);
});

run('没有结构化工具时也用稳定活动行承载 legacy wrapper', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: legacy] {}'),
    toolWrapper(3, 'tool_result', 'legacy output'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(projected[1].mode, 'live');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  const legacy = projected[1].collapsedItems[0];
  assert.equal(legacy.kind, 'tool');
  assert.equal(legacy.tool.summary.verb, 'Legacy');
  assert.equal(legacy.tool.summary.icon, '*');
  assert.match(legacy.tool.output, /\[Tool: legacy\]/);
  assert.match(legacy.tool.output, /legacy output/);
});

run('连续 legacy 工具返回标签在 assistant 文本前折叠成工具摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    { kind: 'msg', id: 2, role: 'tool_result', content: '{"available_skills":["a","b"]}', ts: 2000 },
    { kind: 'msg', id: 3, role: 'tool_result', content: '{"available_categories":["general"]}', ts: 3000 },
    assistant(4, 'continue'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '调用 2 个工具');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[1].collapsedItems.length, 2);
  assert.equal(projected[1].collapsedItems[0].kind, 'tool');
  assert.equal(projected[1].collapsedItems[0].tool.summary.verb, 'Tool');
  assert.match(projected[1].collapsedItems[0].tool.output, /工具调用\n请求未记录/);
  assert.match(projected[1].collapsedItems[0].tool.output, /工具返回\n\{"available_skills"/);
  assert.match(projected[1].collapsedItems[1].tool.output, /工具返回\n\{"available_categories"/);
  assert.equal(JSON.stringify(projected).includes('工具调用 / 返回'), false);
  assert.equal(projected[2].content, 'continue');
});

run('运行中的结构化工具隐藏前置 tool_call 包装行', () => {
  const running = tool(3, { isDone: false, name: 'bash', verb: 'Ran' });
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: bash] {"command":"pnpm test"}'),
    running,
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(projected[1].mode, 'live');
  assert.equal(projected[1].collapsedItems[0].tool.isDone, false);
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
});

run('运行中的结构化工具按 tool_call_id 隐藏调用包装行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: bash] {"command":"pnpm test"}', { tool_call_id: 'call-run' }),
    assistant(3, ' ', 3000, { streaming: true }),
    tool(4, { isDone: false, name: 'bash', verb: 'Ran', object: 'pnpm test', toolCallId: 'call-run' }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(projected[1].mode, 'live');
  const runningTool = projected[1].collapsedItems.find((item) => item.kind === 'tool');
  assert.equal(runningTool.tool.toolCallId, 'call-run');
  assert.equal(runningTool.tool.isDone, false);
  assert.deepEqual([...projected[1].coveredItemIds].sort((a, b) => a - b), [2, 3, 4]);
});

run('完成工具按 tool_call_id 合并调用和返回包装行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: bash] {"command":"pnpm test"}', { tool_call_id: 'call-done' }),
    tool(3, { name: 'bash', verb: 'Ran', object: 'pnpm test', toolCallId: 'call-done' }),
    toolWrapper(4, 'tool_result', 'ok', { tool_call_id: 'call-done' }),
    assistant(5, 'done'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[1].collapsedItems.length, 1);
  assert.equal(projected[1].collapsedItems[0].kind, 'tool');
  assert.equal(projected[1].collapsedItems[0].tool.toolCallId, 'call-done');
  assert.equal(projected[2].content, 'done');
});

run('AskUserQuestion 确认卡片工具不折叠进 activity_summary', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: AskUserQuestion] {}', { tool_call_id: 'call-ask' }),
    askQuestionTool(3),
    toolWrapper(4, 'tool_result', 'User has answered your questions: "Q?"="A"', { tool_call_id: 'call-ask' }),
    assistant(5, 'continue'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'tool', 'msg']);
  assert.equal(projected[1].tool.tool, 'AskUserQuestion');
  assert.equal(projected[1].tool.askUserQuestionResult.items.length, 2);
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[2].content, 'continue');
});

run('同名并行工具按 tool_call_id 各自归并包装行', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: grep] {"q":"A"}', { tool_call_id: 'call-a' }),
    toolWrapper(3, 'tool_call', '[Tool: grep] {"q":"B"}', { tool_call_id: 'call-b' }),
    tool(4, { name: 'grep', verb: 'Read', object: 'A', toolCallId: 'call-a' }),
    tool(5, { name: 'grep', verb: 'Read', object: 'B', toolCallId: 'call-b' }),
    toolWrapper(6, 'tool_result', 'B result', { tool_call_id: 'call-b' }),
    toolWrapper(7, 'tool_result', 'A result', { tool_call_id: 'call-a' }),
    assistant(8, 'done'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].collapsedItems.length, 2);
  const [first, second] = projected[1].collapsedItems;
  assert.equal(first.tool.toolCallId, 'call-a');
  assert.equal(second.tool.toolCallId, 'call-b');
  assert.deepEqual(first.coveredItemIds, [2, 4, 7]);
  assert.deepEqual(second.coveredItemIds, [3, 5, 6]);
  assert.equal(projected[1].collapsedItems.some((item) => item.role === 'tool_call' || item.role === 'tool_result'), false);
});

// ── 子代理调用分组 ───────────────────────────────────────────────
// spawn_subagent / wait_subagent 工具项。metadata.subagent_session_id 是
// 子会话跳转钥匙;wait_subagent 额外把 session_id 放在 args。
function subagentTool(id, {
  name = 'spawn_subagent',
  sessionId = '',
  prompt = '',
  isDone = true,
  ts = id * 1000,
} = {}) {
  return {
    kind: 'tool',
    id,
    ts,
    tool: {
      isDone,
      success: true,
      tool: name,
      summary: { verb: '子代理', object: prompt, metrics: [] },
      args: { prompt, session_id: name === 'wait_subagent' ? sessionId : undefined },
      metadata: sessionId ? { subagent_session_id: sessionId } : null,
      output: '',
    },
  };
}

run('groupSubagentTools 把 spawn/wait 按子会话 id 去重合并成一个分组', () => {
  // 触发场景:fan-out —— 3 次 spawn(wait=false)+ 3 次 wait,共 6 个工具项。
  // 期望行为:按 subagent_session_id 去重成 3 个智能体条目,wait 折叠进来不单列;
  // prompt 由 spawn 提供,coveredItemIds 覆盖全部 6 项。
  const grouped = __test__.groupSubagentTools([
    subagentTool(2, { name: 'spawn_subagent', sessionId: 'A', prompt: '查看 random-fun' }),
    subagentTool(3, { name: 'spawn_subagent', sessionId: 'B', prompt: '查看 cpp_project' }),
    subagentTool(4, { name: 'spawn_subagent', sessionId: 'C', prompt: '查看 random_e6391db9' }),
    subagentTool(5, { name: 'wait_subagent', sessionId: 'A' }),
    subagentTool(6, { name: 'wait_subagent', sessionId: 'B' }),
    subagentTool(7, { name: 'wait_subagent', sessionId: 'C' }),
  ]);

  assert.equal(grouped.length, 1);
  assert.equal(grouped[0].kind, 'subagent_group');
  assert.deepEqual(grouped[0].agents.map((a) => a.sessionId), ['A', 'B', 'C']);
  assert.equal(grouped[0].agents[0].prompt, '查看 random-fun');
  assert.deepEqual(grouped[0].coveredItemIds, [2, 3, 4, 5, 6, 7]);
});

run('groupSubagentTools 不改动非子代理工具，文本边界拆开子代理分组', () => {
  // 触发场景:bash 工具 + spawn A + assistant 文本 + spawn B。
  // 期望行为:bash 原样;assistant 文本结束前一活动段，A、B 不跨段合并。
  const grouped = __test__.groupSubagentTools([
    tool(2, { name: 'bash', verb: 'Ran', object: 'ls' }),
    subagentTool(3, { sessionId: 'A', prompt: 'p' }),
    assistant(4, 'mid'),
    subagentTool(5, { sessionId: 'B', prompt: 'q' }),
  ]);

  assert.deepEqual(grouped.map((i) => i.kind), ['tool', 'subagent_group', 'msg', 'subagent_group']);
  assert.deepEqual(grouped[1].agents.map((a) => a.sessionId), ['A']);
  assert.equal(grouped[2].content, 'mid');
  assert.deepEqual(grouped[3].agents.map((a) => a.sessionId), ['B']);
});

run('spawn 点火与 wait 收结果被可见文本隔开时成为两个活动段', () => {
  // 回归(截图问题):fan-out 4 个子 agent —— 4 次 spawn(wait=false)点火,
  // 中间一段 assistant 推理,再 4 次 wait 收结果。spawn 与 wait 指向同一批
  // 子会话；可见 assistant 文本是稳定活动行的边界，所以保留前后两个分组。
  const grouped = __test__.groupSubagentTools([
    subagentTool(2, { name: 'spawn_subagent', sessionId: 'A', prompt: '看 animal_cat' }),
    subagentTool(3, { name: 'spawn_subagent', sessionId: 'B', prompt: '看 drink_oolong_tea' }),
    subagentTool(4, { name: 'spawn_subagent', sessionId: 'C', prompt: '看 place_kyoto' }),
    subagentTool(5, { name: 'spawn_subagent', sessionId: 'D', prompt: '看 random' }),
    assistant(6, '现在等待四个子代理返回'),
    subagentTool(7, { name: 'wait_subagent', sessionId: 'A' }),
    subagentTool(8, { name: 'wait_subagent', sessionId: 'B' }),
    subagentTool(9, { name: 'wait_subagent', sessionId: 'C' }),
    subagentTool(10, { name: 'wait_subagent', sessionId: 'D' }),
  ]);

  const groups = grouped.filter((i) => i.kind === 'subagent_group');
  assert.equal(groups.length, 2);
  assert.deepEqual(groups[0].agents.map((a) => a.sessionId), ['A', 'B', 'C', 'D']);
  assert.equal(groups[0].agents[0].prompt, '看 animal_cat');
  assert.deepEqual(groups[1].agents.map((a) => a.sessionId), ['A', 'B', 'C', 'D']);
  // 中间的 assistant 推理仍保留一行
  assert.equal(grouped.some((i) => i.kind === 'msg' && i.content === '现在等待四个子代理返回'), true);
});

run('唯一尚未拿到子会话 id 的 spawn 不分组（避免「0 个智能体」）', () => {
  // 触发场景:wait=true 阻塞中,spawn 还没 tool_end,metadata 为空。
  // 期望行为:不聚成 subagent_group,原样保留工具项,等 id 就绪后再分组。
  const running = subagentTool(2, { sessionId: '', prompt: 'p', isDone: false });
  const grouped = __test__.groupSubagentTools([running]);

  assert.equal(grouped.length, 1);
  assert.equal(grouped[0].kind, 'tool');
});

run('reload 后工具名丢失也能靠 metadata.subagent_session_id 识别并分组', () => {
  // 回归:持久化的 tool_result 消息不带工具名字段,重建出 tool.tool=''。
  // 期望行为:仍按 metadata.subagent_session_id 识别为子代理项并分组,标题退到
  // 落盘的 summary.object(spawn 时的 prompt)。
  const reloaded = (id, sessionId, promptObject) => ({
    kind: 'tool',
    id,
    ts: id * 1000,
    tool: {
      isDone: true,
      success: true,
      tool: '',
      args: null,
      summary: { verb: '子代理', object: promptObject, metrics: [] },
      metadata: { subagent_session_id: sessionId },
      output: '',
    },
  });
  const grouped = __test__.groupSubagentTools([
    reloaded(2, 'A', '查看 random-fun'),
    reloaded(3, 'B', '查看 cpp_project'),
  ]);

  assert.equal(grouped.length, 1);
  assert.equal(grouped[0].kind, 'subagent_group');
  assert.deepEqual(grouped[0].agents.map((a) => a.sessionId), ['A', 'B']);
  assert.equal(grouped[0].agents[0].prompt, '查看 random-fun');
});

run('projectCollapsedTranscriptItems 把子代理调用放进统一活动行', () => {
  // 触发场景:一轮里 spawn A/B 后跟一段 assistant 汇总文本。
  // 期望行为:外层仍只有统一 activity_summary，展开后保留 subagent_group 交互。
  const projected = projectCollapsedTranscriptItems([
    user(1),
    subagentTool(2, { sessionId: 'A', prompt: '查看 random-fun' }),
    subagentTool(3, { sessionId: 'B', prompt: '查看 cpp_project' }),
    assistant(4, '汇总完成', 4000, { streaming: true }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((i) => i.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].collapsedItems[0].kind, 'subagent_group');
  assert.equal(projected[1].collapsedItems[0].agents.length, 2);
  assert.equal(projected[1].title, '调用了 2 个智能体');
  assert.equal(projected[2].content, '汇总完成');
});

run('相邻 legacy 调用和返回折叠时保持为一个详情项', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: legacy] {"x":1}'),
    toolWrapper(3, 'tool_result', 'legacy output'),
    assistant(4, 'continue'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].title, '调用 1 个工具');
  assert.equal(projected[1].collapsedItems.length, 1);
  assert.deepEqual(projected[1].collapsedItems[0].coveredItemIds, [2, 3]);
  const legacy = projected[1].collapsedItems[0];
  assert.equal(legacy.kind, 'tool');
  assert.equal(legacy.tool.summary.verb, 'Legacy');
  assert.equal(legacy.tool.summary.object, '1');
  assert.match(legacy.tool.output, /工具调用\n\[Tool: legacy\]/);
  assert.match(legacy.tool.output, /工具返回\nlegacy output/);
  assert.equal(JSON.stringify(legacy).includes('工具调用 / 返回'), false);
});

run('不相邻的 ambiguous legacy wrapper 分别转换但不会被推断合并', () => {
  const normalized = __test__.normalizeToolInvocationItems([
    toolWrapper(2, 'tool_call', '[Tool: legacy] {"x":1}'),
    assistant(3, 'visible separator'),
    toolWrapper(4, 'tool_result', 'legacy output'),
  ]);

  assert.deepEqual(normalized.map((item) => item.kind), ['tool', 'msg', 'tool']);
  assert.equal(normalized[0].tool.summary.object, '1');
  assert.match(normalized[0].tool.output, /结果未记录/);
  assert.equal(normalized[2].tool.summary.verb, 'Tool');
  assert.match(normalized[2].tool.output, /请求未记录/);
});

run('并行同名旧工具按 tool_call_id 匹配各自参数和返回', () => {
  const normalized = __test__.normalizeToolInvocationItems([
    toolWrapper(2, 'tool_call', '[Tool: glob] {"pattern":"A","path":"one"}', { tool_call_id: 'call-a' }),
    toolWrapper(3, 'tool_call', '[Tool: glob] {"pattern":"B","path":"two"}', { tool_call_id: 'call-b' }),
    toolWrapper(4, 'tool_result', 'B result', { tool_call_id: 'call-b' }),
    toolWrapper(5, 'tool_result', 'A result', { tool_call_id: 'call-a' }),
  ]);

  assert.equal(normalized.length, 2);
  assert.deepEqual(normalized.map((item) => item.tool.toolCallId), ['call-a', 'call-b']);
  assert.deepEqual(normalized.map((item) => item.tool.summary.object), ['A · one', 'B · two']);
  assert.match(normalized[0].tool.output, /A result/);
  assert.match(normalized[1].tool.output, /B result/);
});

run('畸形和仅调用记录都转换为结构化工具项', () => {
  const normalized = __test__.normalizeToolInvocationItems([
    toolWrapper(2, 'tool_call', '[Tool: glob] {bad json'),
  ]);

  assert.equal(normalized.length, 1);
  assert.equal(normalized[0].kind, 'tool');
  assert.equal(normalized[0].tool.summary.verb, 'Glob');
  assert.equal(normalized[0].tool.summary.object, '{bad json');
  assert.equal(normalized[0].tool.metadata.legacy_arguments_malformed, true);
  assert.match(normalized[0].tool.output, /结果未记录/);
});

run('正常投影后工具包装角色不会进入旧 SystemRow 路径', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    toolWrapper(2, 'tool_call', '[Tool: glob] {"pattern":"*.js"}'),
    toolWrapper(3, 'tool_result', 'ok'),
    toolWrapper(4, 'tool_result', '[Error] missing request'),
    assistant(5, 'done'),
  ], { deferTrailingToolSummary: true });

  const serialized = JSON.stringify(projected);
  assert.equal(serialized.includes('"role":"tool_call"'), false);
  assert.equal(serialized.includes('"role":"tool_result"'), false);
  assert.equal(serialized.includes('工具调用 / 返回'), false);
  const summary = projected.find((item) => item.kind === 'activity_summary');
  assert.equal(summary.collapsedItems[1].tool.success, false);
});

run('流式遇到 assistant 文字时立刻合并前面的连续工具', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    tool(3, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    tool(4, { verb: 'Created', object: 'c.md', name: 'file_write' }),
    assistant(5, 'I found the files', 5000, { streaming: true }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已创建 1 个文件，读取 2 个文件');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[2].content, 'I found the files');
});

run('流式工具之间的空白 assistant 占位不会拆分工具摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    assistant(3, ' ', 3000, { streaming: true }),
    tool(4, { verb: 'Read', object: 'b.md', name: 'file_read' }),
    assistant(5, '\n', 5000, { streaming: true }),
    tool(6, { verb: 'Created', object: 'c.md', name: 'file_write' }),
    assistant(7, 'Visible answer', 7000, { streaming: true }),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].title, '已创建 1 个文件，读取 2 个文件');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4, 5, 6]);
  assert.equal(projected[2].content, 'Visible answer');
});

run('streaming assistant 开始时把前一工具行切换为汇总但保持活动行', () => {
  const running = tool(2, { isDone: false, name: 'bash', verb: 'Ran' });
  const streaming = assistant(3, 'partial', 3000, { streaming: true });
  const projected = projectCollapsedTranscriptItems([
    user(1),
    running,
    streaming,
  ]);

  assert.equal(projected.length, 3);
  assert.equal(projected[1].kind, 'activity_summary');
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[1].collapsedItems[0], running);
  assert.equal(projected[2], streaming);
});

run('loading、连续工具和文字边界复用同一活动段 key', () => {
  const options = {
    deferTrailingToolSummary: true,
    ensureLiveActivity: true,
    liveTurnId: 'turn-stable',
  };
  const loading = projectCollapsedTranscriptItems([user(1)], options);
  const firstTool = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { isDone: false, name: 'bash', verb: 'Ran', object: 'first' }),
  ], options);
  const secondTool = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { isDone: true, name: 'bash', verb: 'Ran', object: 'first' }),
    tool(3, { isDone: false, name: 'bash', verb: 'Ran', object: 'second' }),
  ], options);
  const textStarted = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { isDone: true, name: 'bash', verb: 'Ran', object: 'first' }),
    tool(3, { isDone: true, name: 'bash', verb: 'Ran', object: 'second' }),
    assistant(4, '开始输出正文', 4000, { streaming: true }),
  ], options);

  const stableId = 'activity:turn:turn-stable:0';
  assert.equal(loading[1].id, stableId);
  assert.equal(firstTool[1].id, stableId);
  assert.equal(secondTool[1].id, stableId);
  assert.equal(textStarted[1].id, stableId);
  assert.equal(loading[1].mode, 'live');
  assert.equal(firstTool[1].mode, 'live');
  assert.equal(secondTool[1].title, 'Ran · second');
  assert.deepEqual(secondTool[1].collapsedItems.map((item) => item.id), [2, 3]);
  assert.equal(textStarted[1].mode, 'tools');
  assert.equal(textStarted[1].title, '已运行 2 条命令，调用 2 个工具');
  assert.equal(textStarted[2].content, '开始输出正文');
  assert.equal(textStarted[3].id, 'activity:turn:turn-stable:1');
  assert.equal(textStarted[3].mode, 'live');
});

run('并行运行工具的收起首行只显示并行数量', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { isDone: false, name: 'file_read', verb: 'Read', object: 'a.md' }),
    tool(3, { isDone: false, name: 'grep', verb: 'Read', object: 'needle' }),
  ], {
    deferTrailingToolSummary: true,
    ensureLiveActivity: true,
    liveTurnId: 'turn-parallel',
  });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary']);
  assert.equal(projected[1].runningToolCount, 2);
  assert.equal(projected[1].title, '正在运行 2 个工具');
});

run('assistant 文本分隔后的下一批工具使用新的稳定活动段 key', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { name: 'file_read', verb: 'Read', object: 'a.md' }),
    assistant(3, '先汇报第一批', 3000, { streaming: true }),
    tool(4, { isDone: false, name: 'bash', verb: 'Ran', object: 'pnpm test' }),
  ], {
    deferTrailingToolSummary: true,
    ensureLiveActivity: true,
    liveTurnId: 'turn-bursts',
  });

  assert.deepEqual(projected.map((item) => item.kind), [
    'msg',
    'activity_summary',
    'msg',
    'activity_summary',
  ]);
  assert.equal(projected[1].id, 'activity:turn:turn-bursts:0');
  assert.equal(projected[1].mode, 'tools');
  assert.equal(projected[3].id, 'activity:turn:turn-bursts:1');
  assert.equal(projected[3].mode, 'live');
});

run('final assistant text 加 task_complete 时保留 final 并折叠前序活动', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    assistant(2, 'I will inspect first'),
    tool(3, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    tool(4, { verb: 'Edited', object: 'src/a.js', name: 'file_edit' }),
    assistant(5, 'Final answer kept'),
    taskComplete(6, 'all done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.match(projected[1].title, /^已处理 /);
  assert.deepEqual(projected[1].coveredItemIds, [2, 3, 4]);
  assert.equal(projected[1].collapsedItems.length, 3);
  assert.equal(projected[2].content, 'Final answer kept');
  assert.equal(projected[3].title, '总结：all done');
  assert.equal(projected.some((item) => item.kind === 'tool' && item.tool?.isTaskComplete), false);
});

run('final assistant text 前的 AskUserQuestion 卡片不折叠进已处理', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    assistant(2, 'I will inspect first'),
    tool(3, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    askQuestionTool(4),
    tool(5, { verb: 'Edited', object: 'src/a.js', name: 'file_edit' }),
    assistant(6, 'Final answer kept'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'tool', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[2].tool.tool, 'AskUserQuestion');
  assert.equal(projected[2].tool.askUserQuestionResult.items.length, 2);
  assert.equal(projected[3].mode, 'processed');
  assert.deepEqual(projected[3].coveredItemIds, [5]);
  assert.equal(projected[4].content, 'Final answer kept');
  assert.equal(projected[1].collapsedItems.some((item) => item.id === 4), false);
  assert.equal(projected[3].collapsedItems.some((item) => item.id === 4), false);
});

run('task_complete 前的 AskUserQuestion 卡片不折叠进已处理', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    assistant(2, 'I will inspect first'),
    tool(3, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    askQuestionTool(4),
    tool(5, { verb: 'Edited', object: 'src/a.js', name: 'file_edit' }),
    assistant(6, 'Final answer kept'),
    taskComplete(7, 'all done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'tool', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[2].tool.tool, 'AskUserQuestion');
  assert.equal(projected[2].tool.askUserQuestionResult.items.length, 2);
  assert.equal(projected[3].mode, 'processed');
  assert.deepEqual(projected[3].coveredItemIds, [5]);
  assert.equal(projected[4].content, 'Final answer kept');
  assert.equal(projected[5].title, '总结：all done');
  assert.equal(projected[1].collapsedItems.some((item) => item.id === 4), false);
  assert.equal(projected[3].collapsedItems.some((item) => item.id === 4), false);
});

run('无 final assistant 时 AskUserQuestion 卡片也不折叠进已处理', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    askQuestionTool(3),
    tool(4, { verb: 'Edited', object: 'src/a.js', name: 'file_edit' }),
    taskComplete(5, 'all done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'tool', 'activity_summary', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2]);
  assert.equal(projected[2].tool.tool, 'AskUserQuestion');
  assert.equal(projected[3].mode, 'processed');
  assert.deepEqual(projected[3].coveredItemIds, [4]);
  assert.equal(projected[4].title, '总结：all done');
  assert.equal(projected[1].collapsedItems.some((item) => item.id === 3), false);
  assert.equal(projected[3].collapsedItems.some((item) => item.id === 3), false);
});

run('大折叠展开内容保留内部工具小折叠层级', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    assistant(2, 'I will inspect first'),
    tool(3, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    tool(4, { verb: 'Read', object: 'src/b.js', name: 'file_read' }),
    assistant(5, 'I found the files'),
    tool(6, { verb: 'Created', object: 'src/c.js', name: 'file_write' }),
    assistant(7, 'Final answer kept'),
    taskComplete(8, 'all done'),
  ]);

  const processed = projected[1];
  assert.equal(processed.kind, 'activity_summary');
  assert.equal(processed.mode, 'processed');
  assert.deepEqual(processed.coveredItemIds, [2, 3, 4, 5, 6]);
  assert.deepEqual(
    processed.detailItems.map((item) => [item.kind, item.mode || item.role, item.title || item.content]),
    [
      ['msg', 'assistant', 'I will inspect first'],
      ['activity_summary', 'tools', '读取 2 个文件'],
      ['msg', 'assistant', 'I found the files'],
      ['activity_summary', 'tools', '已创建 1 个文件'],
    ],
  );
  assert.deepEqual(processed.detailItems[1].coveredItemIds, [3, 4]);
  assert.deepEqual(processed.detailItems[1].collapsedItems.map((item) => item.id), [3, 4]);
  assert.deepEqual(processed.collapsedItems.map((item) => item.id), [2, 3, 4, 5, 6]);
});

run('live complete 到达时保留前一条 streaming assistant 文本', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    assistant(3, 'Final answer still visible while streaming flag remains', 3000, { streaming: true }),
    taskComplete(4, 'all done'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2]);
  assert.equal(projected[2].content, 'Final answer still visible while streaming flag remains');
  assert.equal(projected[2].streaming, true);
  assert.equal(projected[3].title, '总结：all done');
});

run('live complete 摘要形态到达时保留前一条 streaming assistant 文本', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1, 'implement'),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    assistant(3, 'Final answer still visible', 3000, { streaming: true }),
    completeSummaryTool(4, 'all done'),
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2]);
  assert.equal(projected[2].content, 'Final answer still visible');
  assert.equal(projected[3].title, '总结：all done');
});

run('没有 final assistant text 时 task_complete 保留可展开处理摘要', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    taskComplete(3, 'done'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.equal(projected[1].title, '已处理 1s');
  assert.deepEqual(projected[1].coveredItemIds, [2]);
  assert.equal(projected[2].title, '总结：done');
  assert.equal(projected.some((item) => item.kind === 'tool' && item.tool?.isTaskComplete), false);
});

run('task_complete markdown 摘要保留原文给渲染层', () => {
  const summary = '**完成**\n\n- `README.md`\n- docs 已更新';
  const completed = taskComplete(2, summary);
  completed.messageId = 'done-2';
  const projected = projectCollapsedTranscriptItems([
    user(1),
    completed,
  ]);

  assert.equal(projected[1].kind, 'completion_summary');
  assert.equal(projected[1].messageId, 'done-2');
  assert.notEqual(projected[1].messageId, projected[1].id);
  assert.equal(projected[1].summary, summary);
  assert.equal(projected[1].title, '总结：**完成** - `README.md` - docs 已更新');
});

run('complete task 摘要形态不计入工具并显示 summary metric', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'src/a.js', name: 'file_read' }),
    completeSummaryTool(3, '创建helloworld.txt文件，内容为helloworld'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'completion_summary']);
  assert.equal(projected[1].mode, 'processed');
  assert.equal(projected[1].title, '已处理 1s');
  assert.equal(projected[2].title, '总结：创建helloworld.txt文件，内容为helloworld');
});

run('task_complete 的 tool_call 和 tool_result 包装行不会在 live 投影中露出', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, { verb: 'Read', object: 'answers.md', name: 'file_read' }),
    assistant(3, 'Final answer'),
    {
      kind: 'msg',
      id: 4,
      role: 'tool_call',
      content: '[Tool: task_complete] {"summary":"created files"}',
      ts: 4000,
    },
    taskComplete(5, 'created files'),
    {
      kind: 'msg',
      id: 6,
      role: 'tool_result',
      content: 'created files',
      ts: 6000,
    },
  ], { deferTrailingToolSummary: true });

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg', 'completion_summary']);
  assert.equal(projected[1].title, '已处理 3s');
  assert.equal(projected[2].content, 'Final answer');
  assert.equal(projected[3].title, '总结：created files');
  assert.equal(projected.some((item) => item.role === 'tool_call' || item.role === 'tool_result'), false);
});

run('失败工具折叠后外层不显示失败但 expanded 数据保留输出', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    tool(2, {
      name: 'bash',
      verb: 'Ran',
      object: 'bad command',
      success: false,
    }),
    assistant(3, 'retried'),
  ], { deferTrailingToolSummary: true });

  assert.equal(projected[1].kind, 'activity_summary');
  assert.doesNotMatch(projected[1].title, /失败|错误|failed|error/i);
  assert.equal(projected[1].collapsedItems[0].tool.success, false);
  assert.match(projected[1].collapsedItems[0].tool.output, /stderr details/);
});

run('完成态最后一项是 assistant 文本时保留可展开处理摘要和最终文本', () => {
  const projected = projectCollapsedTranscriptItems([
    user(1),
    assistant(2, 'I will inspect'),
    tool(3, { verb: 'Read', object: 'a.md', name: 'file_read' }),
    assistant(4, 'Final visible answer'),
  ]);

  assert.deepEqual(projected.map((item) => item.kind), ['msg', 'activity_summary', 'msg']);
  assert.equal(projected[1].mode, 'processed');
  assert.deepEqual(projected[1].coveredItemIds, [2, 3]);
  assert.equal(projected[2].content, 'Final visible answer');
});

run('同一 raw history 多次投影得到相同可见结构', () => {
  const history = [
    user(1),
    tool(2, { verb: 'Created', object: 'a.txt', name: 'file_write' }),
    assistant(3, 'final'),
    taskComplete(4, 'done'),
  ];
  const reloaded = JSON.parse(JSON.stringify(history));

  const first = projectCollapsedTranscriptItems(history);
  const second = projectCollapsedTranscriptItems(reloaded);

  assert.deepEqual(
    first.map((item) => [item.kind, item.mode || item.role || item.tool?.tool, item.title || item.content || '']),
    second.map((item) => [item.kind, item.mode || item.role || item.tool?.tool, item.title || item.content || '']),
  );
});
