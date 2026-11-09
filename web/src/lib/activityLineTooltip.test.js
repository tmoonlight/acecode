import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { clsx } from './format.js';
import { presentTool } from './toolPresentation.js';

// 从生产组件源码里取出 ActivityLine 编译渲染,避免测到一份手抄的副本。
async function loadActivityLine() {
  const source = readFileSync(new URL('../components/ActivityLine.jsx', import.meta.url), 'utf8');
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const declaration = ast.program.body.map((node) => node.declaration || node)
    .find((node) => node.type === 'FunctionDeclaration' && node.id.name === 'ActivityLine');
  assert.ok(declaration, 'ActivityLine must be tested from the production component');
  const compiled = await transformWithEsbuild(source.slice(declaration.start, declaration.end), 'ActivityLine.jsx', {
    loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
  });
  const VsIcon = ({ name }) => React.createElement('svg', { 'data-icon': name });
  // 服务端渲染不执行 layout effect;换成 useEffect 只为去掉 SSR 警告,不影响被测的标记。
  return vm.runInNewContext(`${compiled.code}; ActivityLine;`, {
    React, ...React, useLayoutEffect: React.useEffect, clsx, VsIcon,
  });
}

const ActivityLine = await loadActivityLine();

function detailTitleOf(html) {
  const match = html.match(/<span[^>]*title="([^"]*)"[^>]*data-activity-detail="true"/);
  assert.ok(match, 'detail span must carry a title');
  return match[1]
    .replace(/&#x([0-9a-f]+);/gi, (_, hex) => String.fromCharCode(parseInt(hex, 16)))
    .replace(/&quot;/g, '"').replace(/&lt;/g, '<').replace(/&gt;/g, '>').replace(/&amp;/g, '&');
}

function run(name, fn) {
  fn();
  console.log(`[pass] ${name}`);
}

const LONG_COMMAND = 'git diff -- web/src/components/SummaryPanel.jsx web/src/lib/summaryPanel.test.js web/src/lib/summaryPanelArchitecture.test.js';

// 回归:transcript 里长命令行的详情文字被截到 80 字加 "...",鼠标停在命令上时
// 弹出的提示也是同一段截断文字(详情 span 自带的 title 盖过了行上的完整命令)。
run('工具行详情的悬停提示显示完整命令,而不是截断后的预览', () => {
  const presentation = presentTool({ tool: 'bash', args: { command: LONG_COMMAND }, isDone: true, success: true });
  // 前提:行内显示的确是截断预览,完整命令保存在 fullObject。
  assert.notEqual(presentation.detail, LONG_COMMAND);
  assert.match(presentation.detail, /\.\.\.$/);
  assert.equal(presentation.fullObject, LONG_COMMAND);

  const html = renderToStaticMarkup(React.createElement(ActivityLine, {
    label: presentation.label,
    detail: presentation.detail,
    detailTitle: presentation.fullObject,
    detailSeparator: presentation.separator,
    title: presentation.fullObject,
    expandable: true,
    onToggle: () => {},
  }));
  assert.equal(detailTitleOf(html), LONG_COMMAND);
  // 行上显示的仍然是截断预览,只有悬停提示变成完整内容。
  assert.ok(html.includes(`>${presentation.detail}</span>`));
});

// 多行命令(heredoc 等)悬停时也要看到全部行,不能只剩第一行。
run('多行命令的悬停提示保留全部行', () => {
  const command = "cat <<'EOF' > a.txt\nfirst line\nsecond line\nEOF";
  const presentation = presentTool({ tool: 'bash', args: { command }, isDone: true, success: true });
  const html = renderToStaticMarkup(React.createElement(ActivityLine, {
    label: presentation.label,
    detail: presentation.detail,
    detailTitle: presentation.fullObject,
    detailSeparator: presentation.separator,
  }));
  assert.equal(detailTitleOf(html), command);
});

// 文件类工具行内只显示文件名,悬停时应看到完整路径。
run('文件工具行悬停提示显示完整路径', () => {
  const filePath = 'N:/Users/shao/acecode/web/src/components/ChatView.jsx';
  const presentation = presentTool({ tool: 'file_read', args: { file_path: filePath }, isDone: true, success: true });
  assert.equal(presentation.detail, 'ChatView.jsx');
  const html = renderToStaticMarkup(React.createElement(ActivityLine, {
    label: presentation.label,
    detail: presentation.detail,
    detailTitle: presentation.fullObject,
    detailSeparator: presentation.separator,
  }));
  assert.equal(detailTitleOf(html), filePath);
});

// 其它调用方(系统提示、编辑摘要等)不传 detailTitle 时,提示仍是 detail 本身。
run('未提供完整原文时悬停提示回退为 detail', () => {
  const html = renderToStaticMarkup(React.createElement(ActivityLine, {
    label: '已编辑', detail: '3 个文件',
  }));
  assert.equal(detailTitleOf(html), '3 个文件');
});

// ToolBlock 的三种工具行(提问结果 / 完成 / 运行中)都必须把完整对象传给详情提示。
run('ToolBlock 的每个带详情的工具行都传入完整原文', () => {
  const tool = readFileSync(new URL('../components/ToolBlock.jsx', import.meta.url), 'utf8');
  const detailRows = tool.match(/detail=\{presentation\.detail\}/g) || [];
  const titledRows = tool.match(/detailTitle=\{presentation\.fullObject\}/g) || [];
  assert.ok(detailRows.length >= 3);
  assert.equal(titledRows.length, detailRows.length);
});
