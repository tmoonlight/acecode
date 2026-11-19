// HTML 可视化编辑的源码定位与精确替换(htmlSourceMap.js)。
// 验收口径:不改动时零字节差异;改一个样式时差异只落在那个标签上;删除 / 改字只动那个元素。
import assert from 'node:assert/strict';
import { parse } from 'parse5';
import {
  HTML_SID_ATTR,
  applyHtmlSplice,
  assignNewHtmlIds,
  attributeSplice,
  collectHtmlElementRecords,
  deleteElementSplice,
  indexHtmlSource,
  innerContentSplice,
  innerSourceText,
  instrumentHtmlRange,
  instrumentHtmlSource,
  lineRangeForOffsets,
  outerSourceText,
  parseStyleDeclarations,
  readSourceAttribute,
  remapHtmlIndex,
  sourceStyleValues,
  styleSplice,
  updateStyleText,
  wrapLinkSplices,
} from './htmlSourceMap.js';

const PAGE = [
  '<!DOCTYPE html>',
  '<!-- 用户注释 pnid: 保留 -->',
  '<html lang=zh>',
  '<head><title>报告</title><style>.a{color:red}</style></head>',
  '<body>',
  "  <h1 class='title' style=\"color: red;  --brand: #f60\">项目 &amp; 数据</h1>",
  '  <p>Hi <b>there</b> <i>you</i></p>',
  '  <p>open',
  '  <div id=box><span>x</span><img src=a.png></div>',
  '  <svg><rect width=4 /></svg>',
  '  <template><li>t</li></template>',
  '</body>',
  '</html>',
].join('\n');

function byTag(index, tag, nth = 0) {
  return [...index.byId.values()].filter((record) => record.tag === tag)[nth];
}

function editOnce(text, index, splice) {
  const next = applyHtmlSplice(text, splice);
  return { text: next, index: remapHtmlIndex(index, splice, collectHtmlElementRecords(next, parse)) };
}

// 场景:打开一份带 doctype、head、注释、SVG、template、省略结束标签的报告页。
// 期望:只给 body 里有源码位置的元素编号;head 里的元素、template 内容、SVG 内部图形不编号;
// <p> 省略结束标签时没有结束标签位置,也就不算可直接改字的文字叶子。
{
  const index = indexHtmlSource(PAGE, parse);
  const tags = [...index.byId.values()].map((record) => record.tag);
  assert.deepEqual(tags, ['h1', 'p', 'b', 'i', 'p', 'div', 'span', 'img', 'svg']);
  assert.ok(index.doctypeEnd > 0 && PAGE.slice(0, index.doctypeEnd) === '<!DOCTYPE html>');
  assert.equal(byTag(index, 'h1').textLeaf, true);
  assert.equal(byTag(index, 'p', 0).textLeaf, true);
  assert.equal(byTag(index, 'p', 1).hasEndTag, false);
  assert.equal(byTag(index, 'p', 1).textLeaf, false);
  assert.equal(byTag(index, 'div').textLeaf, false);
  console.log('[pass] HTML source index numbers body elements only');
}

// 场景:生成送进隔离框的副本。期望:编号插在标签名后面,去掉编号后与原文逐字节相同;
// 原文本身不含编号属性。
{
  const index = indexHtmlSource(PAGE, parse);
  const instrumented = instrumentHtmlSource(PAGE, index);
  assert.ok(instrumented.includes(`<h1 ${HTML_SID_ATTR}="1" class='title'`));
  assert.ok(instrumented.includes(`<img ${HTML_SID_ATTR}="8" src=a.png>`));
  assert.equal(instrumented.replace(/ data-ace-sid="\d+"/g, ''), PAGE);
  assert.ok(!PAGE.includes(HTML_SID_ATTR));
  console.log('[pass] HTML instrumentation only adds sid attributes');
}

// 场景:用工具栏把标题字号改成 30px、去掉颜色。期望:差异只落在 h1 的 style 属性里,
// 原有的单引号 class、CSS 自定义属性、注释和 doctype 原样保留。
{
  const index = indexHtmlSource(PAGE, parse);
  const h1 = byTag(index, 'h1');
  const splice = styleSplice(PAGE, h1, { 'font-size': '30px', color: null });
  const next = applyHtmlSplice(PAGE, splice);
  assert.equal(
    next,
    PAGE.replace('style="color: red;  --brand: #f60"', 'style="--brand: #f60; font-size: 30px"'),
  );
  assert.ok(splice.start > h1.start && splice.end < h1.startTagEnd);
  // 没有 style 的元素:插在标签名后面,不碰其它属性。
  const div = byTag(index, 'div');
  const added = applyHtmlSplice(PAGE, styleSplice(PAGE, div, { 'border-radius': '8px' }));
  assert.equal(added, PAGE.replace('<div id=box>', '<div style="border-radius: 8px" id=box>'));
  // 改回原值时没有替换。
  assert.equal(styleSplice(PAGE, h1, { color: 'red' }), null);
  console.log('[pass] HTML style edits touch only the style attribute');
}

// 场景:声明里有 url(...;...) 和带分号的字符串。期望:按顶层分号切分,改一个声明不破坏另一个。
{
  const decls = parseStyleDeclarations("background: url('a;b.png'); content: \"x;y\"; color:red");
  assert.deepEqual(decls.map((decl) => decl.prop), ['background', 'content', 'color']);
  assert.equal(
    updateStyleText("background: url('a;b.png'); color:red", { color: 'blue' }),
    "background: url('a;b.png'); color: blue",
  );
  assert.equal(updateStyleText('color: red;', { color: null }), '');
  console.log('[pass] HTML style declarations split on top-level semicolons only');
}

// 场景:设置链接地址 / 图片地址,地址里带引号与 &。期望:沿用原引号并正确转义;
// 删除属性时连同前面的空白一起删。
{
  const index = indexHtmlSource(PAGE, parse);
  const img = byTag(index, 'img');
  assert.equal(readSourceAttribute(PAGE, img, 'src').value, 'a.png');
  const next = applyHtmlSplice(PAGE, attributeSplice(PAGE, img, 'src', 'img/b "1".png?x=1&y=2'));
  assert.ok(next.includes('<img src="img/b &quot;1&quot;.png?x=1&amp;y=2">'));
  const h1 = byTag(index, 'h1');
  const noClass = applyHtmlSplice(PAGE, attributeSplice(PAGE, h1, 'class', null));
  assert.ok(noClass.includes('<h1 style="color: red;  --brand: #f60">'));
  assert.equal(readSourceAttribute(PAGE, h1, 'class').quote, "'");
  console.log('[pass] HTML attribute edits keep quoting and escape values');
}

// 场景:双击段落改字后提交。期望:只替换开始标签与结束标签之间的内容;段落后面的元素
// 编号能接回(位置平移),被替换区间里的 <b>/<i> 失去编号,新文字里的 <b> 按先序分配新编号。
{
  const index = indexHtmlSource(PAGE, parse);
  const p = byTag(index, 'p', 0);
  assert.equal(innerSourceText(PAGE, p), 'Hi <b>there</b> <i>you</i>');
  const splice = innerContentSplice(p, 'Hello <b>world</b>');
  const { text, index: remapped } = editOnce(PAGE, index, splice);
  assert.equal(text, PAGE.replace('Hi <b>there</b> <i>you</i>', 'Hello <b>world</b>'));
  assert.equal(remapped.byId.has(p.id), true);
  assert.equal(remapped.byId.has(byTag(index, 'b').id), false);
  assert.equal(remapped.byId.has(byTag(index, 'i').id), false);
  const div = remapped.byId.get(byTag(index, 'div').id);
  assert.equal(outerSourceText(text, div).startsWith('<div id=box>'), true);
  const fresh = collectHtmlElementRecords(text, parse);
  const p2 = remapped.byId.get(p.id);
  const { index: withNew, assigned } = assignNewHtmlIds(remapped, fresh, p2.startTagEnd, p2.endTagStart);
  assert.equal(assigned.length, 1);
  assert.equal(withNew.byId.get(assigned[0]).tag, 'b');
  // 没有结束标签的 <p> 不支持改字。
  assert.equal(innerContentSplice(byTag(index, 'p', 1), 'x'), null);
  console.log('[pass] HTML text edits replace only the element content and remap ids');
}

// 场景:删除独占一行的 <div>。期望:整行连同换行一起删掉,不留空行;后面的 svg 编号接回。
// 删除与别的内容同一行的 <b>:只删元素本身。
{
  const index = indexHtmlSource(PAGE, parse);
  const div = byTag(index, 'div');
  const { text, index: remapped } = editOnce(PAGE, index, deleteElementSplice(PAGE, div));
  assert.equal(text, PAGE.replace('  <div id=box><span>x</span><img src=a.png></div>\n', ''));
  assert.equal(remapped.byId.has(div.id), false);
  assert.equal(remapped.byId.has(byTag(index, 'span').id), false);
  assert.equal(remapped.byId.get(byTag(index, 'svg').id).tag, 'svg');
  const b = byTag(index, 'b');
  assert.equal(applyHtmlSplice(PAGE, deleteElementSplice(PAGE, b)), PAGE.replace('<b>there</b>', ''));
  console.log('[pass] HTML element deletion removes whole lines only when the element owns them');
}

// 场景:给标题整体加链接。期望:两次纯插入,标题里原有的文字不动;标题与之后元素的编号都接回。
{
  const index = indexHtmlSource(PAGE, parse);
  const p = byTag(index, 'p', 0);
  let text = PAGE;
  let current = index;
  for (const splice of wrapLinkSplices(p, 'https://example.com/?a=1&b=2')) {
    ({ text, index: current } = editOnce(text, current, splice));
  }
  assert.ok(text.includes('<p><a href="https://example.com/?a=1&amp;b=2">Hi <b>there</b> <i>you</i></a></p>'));
  assert.equal(current.byId.get(byTag(index, 'b').id).tag, 'b');
  assert.equal(current.byId.get(byTag(index, 'img').id).tag, 'img');
  console.log('[pass] HTML link wrapping keeps descendant ids');
}

// 场景:AI 编辑把元素的行号带给模型。期望:行号从 1 开始,跨行元素给出起止行。
{
  const index = indexHtmlSource(PAGE, parse);
  const h1 = byTag(index, 'h1');
  assert.deepEqual(lineRangeForOffsets(PAGE, h1.start, h1.end), { startLine: 6, endLine: 6, lineCount: 1 });
  const text = 'a\n<div>\n x\n</div>\n';
  assert.deepEqual(lineRangeForOffsets(text, 2, 17), { startLine: 2, endLine: 4, lineCount: 3 });
  console.log('[pass] HTML element line ranges are 1-based');
}

// 场景:撤销改字时在框内原地换回元素内容。期望:只取元素内容这一段,并给其中的子元素插上
// 当时编号表里的编号(与框内其它元素的编号一致),段外的元素不插。
{
  const index = indexHtmlSource(PAGE, parse);
  const p = byTag(index, 'p', 0);
  const b = byTag(index, 'b');
  const i = byTag(index, 'i');
  assert.equal(
    instrumentHtmlRange(PAGE, index, p.startTagEnd, p.endTagStart),
    `Hi <b ${HTML_SID_ATTR}="${b.id}">there</b> <i ${HTML_SID_ATTR}="${i.id}">you</i>`,
  );
  assert.equal(instrumentHtmlRange(PAGE, index, 0, PAGE.length), instrumentHtmlSource(PAGE, index));
  console.log('[pass] HTML range instrumentation restores element content with matching ids');
}

// 场景:撤销改样式要把那几个声明还原成修改前的值。期望:读出源码 style 里这几个声明当前的值,
// 原来没有的给 null(还原时删除),大小写不敏感。
{
  const index = indexHtmlSource(PAGE, parse);
  const h1 = byTag(index, 'h1');
  assert.deepEqual(sourceStyleValues(PAGE, h1, ['color', 'font-size', '--brand']),
    { color: 'red', 'font-size': null, '--brand': '#f60' });
  assert.deepEqual(sourceStyleValues(PAGE, byTag(index, 'div'), ['border-radius']), { 'border-radius': null });
  console.log('[pass] HTML style values before an edit are read from the source');
}
