import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { renderMarkdown, renderMarkdownBlocks } from './markdown.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 核心不变量:按块渲染的 HTML 拼接必须与全文渲染逐字节一致。全文只
// parse 一次再按 top-level token 组分别 render,所以两者理应等价 ——
// 一旦分组逻辑漏 token 或切错边界,这条会最先失败。
run('block html concatenation equals whole-document render', () => {
  const src = [
    '# 标题',
    '',
    '第一段,有 **加粗** 和 `inline code`。',
    '',
    '```js',
    'const x = 1;',
    '```',
    '',
    '- 列表项 1',
    '- 列表项 2',
    '  - 嵌套项',
    '',
    '| a | b |',
    '| - | - |',
    '| 1 | 2 |',
    '',
    '> 引用块',
    '',
    '---',
    '',
    '结尾段落 <script>alert(1)</script>',
  ].join('\n');
  const blocks = renderMarkdownBlocks(src);
  assert.ok(blocks.length >= 7, `期望切出多个块,实际 ${blocks.length}`);
  assert.equal(blocks.map((b) => b.html).join(''), renderMarkdown(src));
});

run('lists preserve starts, nesting, and task checkbox semantics', () => {
  const ordered = renderMarkdown([
    '3. 外层第三项',
    '4. 外层第四项',
    '   1. 嵌套第一项',
  ].join('\n'));
  assert.match(ordered, /<ol start="3">/);
  assert.equal((ordered.match(/<ol(?:\s[^>]*)?>/g) || []).length, 2, ordered);
  assert.match(ordered, /<ol start="3">[\s\S]*<ol>[\s\S]*<\/ol>[\s\S]*<\/ol>/);

  const unordered = renderMarkdown('- 外层\n  - 嵌套');
  assert.equal((unordered.match(/<ul(?:\s[^>]*)?>/g) || []).length, 2, unordered);

  const tasks = renderMarkdown('- [ ] 待办\n- [x] 完成');
  assert.match(tasks, /<ul class="contains-task-list">/);
  assert.equal((tasks.match(/class="task-list-item"/g) || []).length, 2, tasks);
  assert.equal((tasks.match(/type="checkbox"/g) || []).length, 2, tasks);
  assert.equal((tasks.match(/checked=""/g) || []).length, 1, tasks);
});

run('shared markdown styles restore list markers in file previews', () => {
  const styles = source('styles/globals.css');
  assert.match(styles, /\.ace-md ul\s*\{[^}]*list-style-type:\s*disc\s*;/s);
  assert.match(styles, /\.ace-md ol\s*\{[^}]*list-style-type:\s*decimal\s*;/s);
  assert.match(styles, /\.ace-md li\.task-list-item\s*\{[^}]*list-style:\s*none\s*;/s);

  const preview = source('components/FilePreviewContent.jsx');
  assert.match(preview, /className="h-full overflow-auto ace-md ace-side-markdown-preview"/);
  assert.match(preview, /dangerouslySetInnerHTML=\{\{ __html: renderMarkdown\(previewText\) \}\}/);
});

// 回归测试(fix 流式出字时消息区上下跳动):流式 append 时,已定稿的
// 前缀块 HTML 字符串必须保持不变 —— React 靠这个字符串比较跳过 DOM
// 更新,前缀 DOM 不动才不会破坏滚动稳定性。
run('streaming append keeps finished prefix block html unchanged', () => {
  const prefix = '第一段。\n\n```python\nprint("hi")\n```\n\n第二段完整。\n\n';
  const before = renderMarkdownBlocks(`${prefix}正在输出的第三`);
  const after = renderMarkdownBlocks(`${prefix}正在输出的第三段继续变长,还带 **格式**。`);
  assert.ok(before.length >= 4);
  assert.equal(after.length, before.length);
  for (let i = 0; i < before.length - 1; i += 1) {
    assert.equal(after[i].html, before[i].html, `前缀块 ${i} 的 HTML 不应随流式追加变化`);
    assert.equal(after[i].key, before[i].key, `前缀块 ${i} 的 key 不应随流式追加变化`);
  }
  assert.notEqual(after[after.length - 1].html, before[before.length - 1].html);
});

// 场景:reference link 的使用和定义分在不同块([text][ref] 在第一段,
// [ref]: url 在文末)。切块渲染共享同一个 parse env,引用必须仍能解析
// 成 <a>,不能退化成字面方括号文本。
run('reference links resolve across block boundaries', () => {
  const src = '看这个 [链接][ref] 的效果。\n\n另一段。\n\n[ref]: https://example.com';
  const joined = renderMarkdownBlocks(src).map((b) => b.html).join('');
  assert.ok(joined.includes('href="https://example.com"'), joined);
});

// 边界:空输入 / 非字符串输入不产生块;纯文本单段落切出恰好一块。
run('empty and trivial inputs', () => {
  assert.deepEqual(renderMarkdownBlocks(''), []);
  assert.deepEqual(renderMarkdownBlocks(null), []);
  const single = renderMarkdownBlocks('只有一段');
  assert.equal(single.length, 1);
  assert.equal(single[0].html, renderMarkdown('只有一段'));
});

// 场景:流式中途的未闭合代码围栏(``` 开了没关)。markdown-it 会把其后
// 内容全部并入 fence 直到文末 —— 未闭合 fence 应落在最后一块里正常渲染,
// 且拼接仍与全文渲染一致(不崩、不丢内容)。
run('unclosed streaming code fence stays consistent with whole render', () => {
  const src = '前一段。\n\n```cpp\nint main() {\n  return 0;';
  const blocks = renderMarkdownBlocks(src);
  assert.equal(blocks.map((b) => b.html).join(''), renderMarkdown(src));
  // fence 内容会被 highlight.js 包 span,断言块结构而不是字面代码文本
  const tail = blocks[blocks.length - 1].html;
  assert.ok(tail.includes('language-cpp'), tail);
  assert.ok(tail.includes('main'), tail);
});

// 回归测试(WebView 长时间跑 Agent 后渲染进程涨到 1–2 GB):流式中尾部未闭合
// 的代码块必须交给 React 渲染成单个文本节点,不能每个 token 都整块重新高亮、
// 整块替换 innerHTML —— 那会反复创建几百个 span,把 Blink 的 DOM 堆撑大。
// 期望:streaming 模式下最后一块是 openFence(语言归一、原文不转义),前面的块
// 与非流式 HTML 逐字节一致;非流式调用保持旧行为。
run('streaming tail open fence becomes a plain text block', () => {
  const src = '前一段。\n\n```ts\nconst a = 1 < 2;\nconst b = "x";';
  const streamingBlocks = renderMarkdownBlocks(src, { streaming: true });
  const normalBlocks = renderMarkdownBlocks(src);
  assert.equal(streamingBlocks.length, normalBlocks.length);
  const tail = streamingBlocks[streamingBlocks.length - 1];
  assert.equal(tail.html, undefined);
  assert.deepEqual(tail.openFence, { lang: 'typescript', text: 'const a = 1 < 2;\nconst b = "x";' });
  assert.equal(streamingBlocks[0].html, normalBlocks[0].html);
  assert.ok(normalBlocks[normalBlocks.length - 1].html.includes('hljs-keyword'));
});

// 场景:流式中代码块已经闭合(后面还在输出正文),或语言 highlight.js 不认识。
// 期望:已闭合的块照常高亮、走 html;未知语言的未闭合块 lang 为空,样式退回
// 普通 <pre><code>,和非流式渲染的选择一致。
run('streaming keeps closed fences highlighted and drops unknown languages', () => {
  const closed = renderMarkdownBlocks('```js\nlet x = 1;\n```\n\n正在输出', { streaming: true });
  assert.ok(closed[0].html.includes('hljs-keyword'), closed[0].html);
  assert.equal(closed[closed.length - 1].openFence, undefined);

  const unknown = renderMarkdownBlocks('```foolang\nabc', { streaming: true });
  assert.deepEqual(unknown[0].openFence, { lang: '', text: 'abc' });
});

// 场景:未闭合代码块嵌在列表里(不是独立的尾块),只能整块走 html。
// 期望:流式时它不高亮(没有 hljs-* span),但保留 language class;
// 非流式同一段仍高亮,完成后的消息不受影响。
run('streaming renders nested open fences without highlight spans', () => {
  const src = '- 第一步\n\n  ```cpp\n  int main() { return 0; }';
  const streamingHtml = renderMarkdownBlocks(src, { streaming: true }).map((b) => b.html).join('');
  assert.ok(streamingHtml.includes('language-cpp'), streamingHtml);
  assert.ok(!streamingHtml.includes('hljs-'), streamingHtml);
  const normalHtml = renderMarkdownBlocks(src).map((b) => b.html).join('');
  assert.ok(normalHtml.includes('hljs-'), normalHtml);
});

// 架构守护:AssistantBubble 必须对流式内容限频后再切块,并把 openFence 交给
// StreamingCodeBlock 渲染成文本节点;去掉任何一环,流式出字就会回到逐 token
// 全文解析 + 整块替换 DOM。
run('assistant bubble throttles streaming markdown and renders open fence as text', () => {
  const message = source('components/Message.jsx');
  assert.match(message, /useStreamingRenderThrottle\(content \|\| '', !!streaming\)/);
  assert.match(message, /renderMarkdownBlocks\(renderedContent, \{ streaming: !!streaming \}\)/);
  assert.match(message, /block\.openFence \? \(\s*<StreamingCodeBlock/);
  assert.match(message, /data-code-copy-source="true">\{text\}<\/code>/);
});
