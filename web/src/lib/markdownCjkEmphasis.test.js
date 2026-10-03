import assert from 'node:assert/strict';
import { renderMarkdown, renderMarkdownBlocks, renderMarkdownInline } from './markdown.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const emphasisCases = [
  ['**建议：**共享', '<strong>建议：</strong>共享'],
  ['前文**（重点）**后文', '前文<strong>（重点）</strong>后文'],
  ['*注意：*继续', '<em>注意：</em>继续'],
  ['~~旧值：~~新值', '<s>旧值：</s>新值'],
  ['**注意。**続き', '<strong>注意。</strong>続き'],
  ['**주의。**다음', '<strong>주의。</strong>다음'],
  ['提示：**(可选)**', '提示：<strong>(可选)</strong>'],
  ['结果：**"通过"**', '结果：<strong>&quot;通过&quot;</strong>'],
  ['中文，__重点__。', '中文，<strong>重点</strong>。'],
  ['**建议：** 共享', '<strong>建议：</strong> 共享'],
  ['**建议**：共享', '<strong>建议</strong>：共享'],
  ['**bold** and *italic* and ~~old~~', '<strong>bold</strong> and <em>italic</em> and <s>old</s>'],
  ['**Note:**text', '**Note:**text'],
  ['some_variable_name', 'some_variable_name'],
  ['** 建议：**共享', '** 建议：**共享'],
  ['**建议： **共享', '**建议： **共享'],
];

for (const [source, expected] of emphasisCases) {
  run(`CJK emphasis keeps inline, document, and block output consistent: ${source}`, () => {
    assert.equal(renderMarkdownInline(source), expected);
    assert.equal(renderMarkdown(source), `<p>${expected}</p>\n`);
    assert.equal(renderMarkdownBlocks(source).map((block) => block.html).join(''), `<p>${expected}</p>\n`);
  });
}

run('CJK list labels render in bold beside inline code', () => {
  const labels = ['同会话消息排序', '投递账本', '手机控制面', '桌面可观测性', '统一帮助', '第二个平台明确后再抽公共契约'];
  const source = labels.map((label) => `- **${label}：**继续处理 \`status\``).join('\n');
  const html = renderMarkdown(source);
  for (const label of labels) {
    assert.ok(html.includes(`<li><strong>${label}：</strong>继续处理 <code>status</code></li>`), html);
  }
  assert.equal(renderMarkdownBlocks(source).map((block) => block.html).join(''), html);
});

run('CJK emphasis preserves literal code, escaped markers, and HTML escaping', () => {
  assert.equal(renderMarkdownInline('`**建议：**共享`'), '<code>**建议：**共享</code>');
  assert.equal(renderMarkdownInline(String.raw`\*\*建议：\*\*共享`), '**建议：**共享');
  const code = renderMarkdown('```\n**建议：**共享\n```');
  assert.ok(code.includes('>**建议：**共享\n</code>'), code);
  assert.ok(!code.includes('<strong>'), code);
  assert.equal(
    renderMarkdownInline('**建议：**<script>alert(1)</script>'),
    '<strong>建议：</strong>&lt;script&gt;alert(1)&lt;/script&gt;',
  );
  const unsafeLink = renderMarkdown('**建议：**[链接](javascript:alert(1))');
  assert.ok(!unsafeLink.includes('<a '), unsafeLink);
});

run('streaming CJK emphasis resolves when closed without changing finished blocks', () => {
  const prefix = '前一段已完成。\n\n';
  const source = '**建议：**共享';
  const prefixHtml = renderMarkdown(prefix);
  assert.ok(!renderMarkdownInline('**建议：').includes('<strong>'));
  assert.equal(renderMarkdownInline('**建议：**'), '<strong>建议：</strong>');
  for (let length = 1; length <= source.length; length += 1) {
    const streamed = prefix + source.slice(0, length);
    const blocks = renderMarkdownBlocks(streamed);
    assert.equal(blocks[0].html, prefixHtml);
    assert.equal(blocks.map((block) => block.html).join(''), renderMarkdown(streamed));
  }
  const finalBlocks = renderMarkdownBlocks(prefix + source);
  assert.equal(finalBlocks.at(-1).html, '<p><strong>建议：</strong>共享</p>\n');
});
