import assert from 'node:assert/strict';
import { markdownRoundTrip, markdownToSlate, slateToMarkdown } from './markdownWysiwyg.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function walk(nodes, visit) {
  for (const node of Array.isArray(nodes) ? nodes : []) {
    visit(node);
    if (Array.isArray(node?.children)) walk(node.children, visit);
  }
}

run('Markdown editor model covers common block, inline, task and table text nodes', () => {
  const source = [
    '# 标题',
    '',
    '正文 **粗体**、_斜体_、~~删除~~、`code` 和 [链接](https://example.com)。',
    '',
    '> 引用',
    '',
    '- 普通项',
    '- [x] 完成项',
    '',
    '1. 第一项',
    '2. 第二项',
    '',
    '| 名称 | 值 |',
    '| --- | ---: |',
    '| A | 1 |',
    '',
    '```js',
    'const value = 1;',
    '```',
  ].join('\n');
  const document = markdownToSlate(source);
  const types = [];
  const marked = { bold: false, italic: false, strike: false, code: false };
  walk(document, (node) => {
    if (node?.type) types.push(node.type);
    for (const key of Object.keys(marked)) {
      if (node?.[key]) marked[key] = true;
    }
  });
  for (const type of [
    'heading', 'paragraph', 'link', 'blockquote', 'list', 'list-item',
    'table', 'table-row', 'table-cell', 'code-block',
  ]) {
    assert.ok(types.includes(type), `missing ${type}`);
  }
  assert.deepEqual(marked, { bold: true, italic: true, strike: true, code: true });
  assert.equal(
    document.find((node) => node.type === 'list')?.children[1]?.checked,
    true,
  );

  const output = slateToMarkdown(document);
  const reparsed = markdownToSlate(output);
  const reparsedTypes = [];
  walk(reparsed, (node) => { if (node?.type) reparsedTypes.push(node.type); });
  assert.ok(reparsedTypes.includes('table'));
  assert.ok(reparsedTypes.includes('code-block'));
  assert.match(output, /\[x\] 完成项/i);
  assert.match(output, /https:\/\/example\.com/);
});

run('Markdown editor preserves opaque Mermaid, math, HTML, images and footnotes', () => {
  const source = [
    '正文含公式 $x^2$ 和图片 ![示意](assets/demo.png)。',
    '',
    '<custom-widget value="1">原文</custom-widget>',
    '',
    '```mermaid',
    'graph TD',
    '  A --> B',
    '```',
    '',
    '带脚注[^note]。',
    '',
    '[^note]: 保留脚注定义',
  ].join('\n');
  const document = markdownToSlate(source);
  const opaque = [];
  walk(document, (node) => {
    if (node?.type === 'opaque-block' || node?.type === 'opaque-inline') opaque.push(node.raw);
  });
  assert.ok(opaque.some((raw) => raw === '$x^2$'));
  assert.ok(opaque.some((raw) => raw === '![示意](assets/demo.png)'));
  assert.ok(opaque.some((raw) => raw.includes('<custom-widget')));
  assert.ok(opaque.some((raw) => raw.includes('```mermaid')));
  assert.ok(opaque.some((raw) => raw === '[^note]'));
  assert.ok(opaque.some((raw) => raw.includes('[^note]: 保留脚注定义')));

  const output = slateToMarkdown(document);
  for (const raw of opaque) assert.ok(output.includes(raw), `opaque source missing: ${raw}`);
});

run('Markdown editor keeps a legal paragraph for empty input', () => {
  const document = markdownToSlate('');
  assert.deepEqual(document, [{ type: 'paragraph', children: [{ text: '' }] }]);
  assert.equal(markdownRoundTrip(''), '');
});
