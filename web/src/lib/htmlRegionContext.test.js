// HTML 预览里拖出的选区 → 输入框「选区」引用(htmlRegionContext.js)。
import assert from 'node:assert/strict';
import { parse } from 'parse5';
import {
  HTML_REGION_ELEMENT_LIMIT,
  createHtmlRegionContext,
  normalizeHtmlRegion,
} from './htmlRegionContext.js';
import { indexHtmlSource } from './htmlSourceMap.js';
import { contextPresentation, normalizeComposerContext } from './selectionChatContext.js';

const PAGE = [
  '<!DOCTYPE html>',
  '<html><body>',
  '  <h2 class="title">季度对比</h2>',
  '  <p class="note">排序只影响展示顺序</p>',
  '  <div id="rows"></div>',
  '</body></html>',
].join('\n');

function sidOf(index, tag) {
  for (const [sid, record] of index.byId) if (record.tag === tag) return sid;
  return 0;
}

// 触发场景:框内页面脚本伪造 region 消息,塞进超长字符串、非法标签名、几百个元素、非数字坐标。
// 期望行为:只留已知字段;坐标取整、非数字当 0;元素最多 30 个;标签名不合法的元素丢掉;宽高为 0 的选区作废。
{
  const elements = Array.from({ length: 80 }, (_, i) => ({ sid: i + 1, tag: 'div', text: 'x'.repeat(500), extra: 1 }));
  elements.unshift({ tag: '<img onerror=x>' });
  const region = normalizeHtmlRegion({
    rect: { x: 10.4, y: '20', w: 300, h: 120, vw: 1000, vh: 'NaN' },
    page: { x: 10, y: 1220, w: 300, h: 120 },
    scroll: { x: 0, y: 1200 },
    elements,
    evil: true,
  });
  assert.deepEqual(region.rect, { x: 10, y: 20, w: 300, h: 120, vw: 1000, vh: 0 });
  assert.equal(region.elements.length, HTML_REGION_ELEMENT_LIMIT - 1);
  assert.equal(region.elements[0].text.length, 200);
  assert.equal('extra' in region.elements[0], false);
  assert.equal('evil' in region, false);
  assert.equal(normalizeHtmlRegion({ rect: { x: 1, y: 1, w: 0, h: 10 } }), null);
  assert.equal(normalizeHtmlRegion(null), null);
  console.log('[pass] HTML region messages from the frame are sanitized');
}

// 场景:用户在页面上拖出一块选区,里面有一个源码里的标题、一个脚本生成的表格行,点「AI 编辑」。
// 期望行为:引用的标签是「选区」、图标是取景框;正文写文件路径与选区位置,源码里的元素带行号与源码片段,
// 脚本生成的元素注明源码里没有并带选择器与运行时 HTML;经输入框的规范化后正文仍在。
{
  const index = indexHtmlSource(PAGE, parse);
  const context = createHtmlRegionContext({
    path: 'N:\\work\\report\\index.html',
    source: PAGE,
    index,
    now: 1,
    region: {
      rect: { x: 80, y: 210, w: 430, h: 355, vw: 1000, vh: 800 },
      page: { x: 80, y: 1310, w: 430, h: 355 },
      scroll: { x: 0, y: 1100 },
      elements: [
        { sid: sidOf(index, 'h2'), tag: 'h2', text: '季度对比' },
        { sid: 0, anchorSid: sidOf(index, 'div'), tag: 'tr', selector: 'div#rows > tr:nth-of-type(4)', text: '春分活动 已完成', html: '<tr><td>春分活动</td></tr>', relation: 'partial' },
        { sid: 0, tag: 'div', selector: 'div.wrap', text: '', html: '<div class="wrap"></div>', relation: 'container' },
      ],
    },
  });
  assert.equal(context.type, 'browser');
  assert.equal(context.kind, 'region');
  assert.equal(context.label, '选区');
  assert.equal(context.note, 'index.html');
  assert.ok(context.content.includes('File: N:\\work\\report\\index.html'));
  assert.ok(context.content.includes('left 80, top 1310, width 430, height 355'));
  assert.ok(context.content.includes('1. <h2> at line 3'));
  assert.ok(context.content.includes('<h2 class="title">季度对比</h2>'));
  assert.ok(context.content.includes('2. <tr> rendered at runtime'));
  assert.ok(context.content.includes('Selector: div#rows > tr:nth-of-type(4)'));
  assert.ok(context.content.includes('Nearest element in the source: <div> at line 5'));
  assert.ok(context.content.includes('only partly inside the region'));
  assert.ok(context.content.includes("3. <div> rendered at runtime by the page's script, not present in the source (the region lies inside this element)"));
  const composer = normalizeComposerContext(context);
  assert.equal(composer.kind, 'region');
  assert.equal(composer.content, context.content);
  const presentation = contextPresentation(context);
  assert.equal(presentation.icon, 'RegionSelect');
  assert.equal(presentation.label, '选区');
  assert.equal(presentation.title, '选区\nindex.html');
  console.log('[pass] HTML region becomes a region reference with source lines');
}

// 场景:框里报来的编号对不上源码(脚本克隆出重复编号、或标签不一致)。期望:不按源码行号描述,
// 当作运行时生成的元素处理;选区里没有元素时如实说明。没有文件路径时不生成引用。
{
  const index = indexHtmlSource(PAGE, parse);
  const rect = { x: 0, y: 0, w: 50, h: 50, vw: 800, vh: 600 };
  const duplicate = createHtmlRegionContext({
    path: '/w/index.html', source: PAGE, index,
    region: { rect, elements: [{ sid: sidOf(index, 'h2'), tag: 'h2', duplicate: true, selector: 'h2' }] },
  });
  assert.ok(duplicate.content.includes('rendered at runtime'));
  const wrongTag = createHtmlRegionContext({
    path: '/w/index.html', source: PAGE, index,
    region: { rect, elements: [{ sid: sidOf(index, 'h2'), tag: 'p', selector: 'p' }] },
  });
  assert.ok(wrongTag.content.includes('rendered at runtime'));
  const empty = createHtmlRegionContext({ path: '/w/index.html', source: PAGE, index, region: { rect, elements: [] } });
  assert.ok(empty.content.includes('No element lies in the region'));
  assert.equal(createHtmlRegionContext({ path: '', source: PAGE, index, region: { rect } }), null);
  console.log('[pass] HTML region falls back to runtime descriptions when ids do not match the source');
}
