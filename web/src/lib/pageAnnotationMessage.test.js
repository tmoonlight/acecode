import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  annotationShotsByName,
  contentPartsWithoutAnnotationScreenshots,
  pageAnnotationCardFromContexts,
  pageAnnotationMessageCopyText,
  pageAnnotationScreenshot,
  parsePageAnnotationMessage,
} from './pageAnnotationMessage.js';

// v1：旧消息只有批注，头部在第一行。
const legacy = `Page annotations\n${JSON.stringify({
  page: { url: 'http://127.0.0.1:8765/moon-garden.html', title: '月面温室档案 / 2049' },
  annotations: [{
    number: 3,
    description: '这里文字加粗',
    targets: [{ id: 'target-10', tag: 'p', locator: 'p', text: '整段元素原文', bounds: { x: 20, y: 59, width: 560, height: 93 } }],
    region: null,
    differences: [{ property: 'font-weight', old_value: '400', new_value: '700' }],
  }, { number: 4, description: '   ', targets: [], region: null, differences: [] }],
})}`;
const card = parsePageAnnotationMessage(legacy);
assert.equal(card.text, '');
assert.equal(card.title, '月面温室档案 / 2049');
assert.deepEqual(card.comments.map(({ number, element, description }) => ({ number, element, description })),
  [{ number: 3, element: 'p', description: '这里文字加粗' }]);
assert.equal(pageAnnotationMessageCopyText(card), 'p 这里文字加粗');
assert.equal(JSON.stringify(card).includes('整段元素原文'), false);
assert.equal(JSON.stringify(card).includes('font-weight'), false);

// v2：用户正文 + 空行 + 批注载荷，多页面、每页一张整屏图。
const modern = `把这两页的间距调一下\n\nPage annotations\n${JSON.stringify({
  version: 2,
  pages: [{
    page_id: 'browser-32080-3',
    url: 'https://a.example/', title: '商品详情页', shot: 'page-annotations-c1-1.png',
    annotations: [
      { number: 1, element: 'div', description: '卡片间距太小', state: 'live' },
      { number: 2, element: 'input', description: '输入框边框太浅', state: 'orphan' },
    ],
  }, {
    page_id: 'browser-32080-4',
    url: 'https://b.example/', title: '结算页', shot: 'page-annotations-c1-2.png',
    annotations: [{ number: 1, element: 'p', description: '这段文案要改', state: 'live' }],
  }],
})}`;
const multi = parsePageAnnotationMessage(modern);
assert.equal(multi.text, '把这两页的间距调一下');
assert.deepEqual(multi.pages.map((page) => page.title), ['商品详情页', '结算页']);
assert.deepEqual(multi.pages.map((page) => page.shot), ['page-annotations-c1-1.png', 'page-annotations-c1-2.png']);
assert.equal(multi.comments.length, 3);
assert.deepEqual(multi.comments.map((comment) => comment.element), ['div', 'input', 'p']);
assert.equal(pageAnnotationMessageCopyText(multi),
  '把这两页的间距调一下\n\n商品详情页\n1 div 卡片间距太小\n2 input 输入框边框太浅\n结算页\n1 p 这段文案要改');

const parts = [
  { type: 'image', attachment: { id: 'shot', name: 'page-annotations-c1-1.png', mime_type: 'image/png', blob_url: '/shot.png' } },
  { type: 'image', attachment: { id: 'other', name: 'photo.png', mime_type: 'image/png', blob_url: '/photo.png' } },
];

// 新消息：批注只存在于 browser_context，正文保持自然语言。
const contextParts = [{
  type: 'browser_context',
  context: {
    type: 'page_annotations',
    version: 2,
    pages: [{
      page_id: 'browser-32080-3',
      url: 'https://a.example/', title: '商品详情页', shot: 'page-annotations-c1-1.png',
      annotations: [{ number: 1, element: 'div', description: '卡片间距太小', state: 'live' }],
    }],
  },
}, ...parts];
const contextual = pageAnnotationCardFromContexts(contextParts);
assert.equal(contextual.text, '');
assert.equal(contextual.fromContext, true);
assert.deepEqual(contextual.pages.map((page) => page.title), ['商品详情页']);
assert.deepEqual(contextual.comments.map((comment) => comment.description), ['卡片间距太小']);
assert.equal(annotationShotsByName(contextParts).get('page-annotations-c1-1.png').id, 'shot');
assert.equal(pageAnnotationMessageCopyText(contextual, '只显示自然语言正文'),
  '只显示自然语言正文\n\ndiv 卡片间距太小');
assert.equal(pageAnnotationCardFromContexts(parts), null);

assert.equal(parsePageAnnotationMessage('Page annotations\nnot-json'), null);
assert.equal(parsePageAnnotationMessage('普通消息'), null);
// 正文里出现同名字样但后面不是载荷：不能误判成批注消息。
assert.equal(parsePageAnnotationMessage('我提到了 Page annotations\n这句话'), null);

assert.equal(pageAnnotationScreenshot(parts).id, 'shot');
assert.deepEqual([...annotationShotsByName(parts).keys()], ['page-annotations-c1-1.png']);
assert.deepEqual(contentPartsWithoutAnnotationScreenshots(parts).map((part) => part.attachment.id), ['other']);

const message = fs.readFileSync(new URL('../components/Message.jsx', import.meta.url), 'utf8');
assert.match(message, /parsePageAnnotationMessage/);
assert.match(message, /pageAnnotationCardFromContexts/);
assert.match(message, /PageAnnotationMessageCard/);
const cardComponent = fs.readFileSync(new URL('../components/PageAnnotationMessageCard.jsx', import.meta.url), 'utf8');
assert.match(cardComponent, /条注释/);
assert.match(cardComponent, /shots/);
assert.doesNotMatch(cardComponent, /onEdit|onDelete|回到页面修改|铅笔|垃圾桶/,
  'sent annotation cards stay read-only');
const annotationStyles = fs.readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
const cardStyle = annotationStyles.match(/\.ace-page-annotation-message\s*\{([^}]*)\}/s)?.[1] || '';
assert.doesNotMatch(cardStyle, /overflow:\s*hidden/,
  'the card must not clip its hover detail');
const shotStyle = annotationStyles.match(/\.ace-page-annotation-message-shot\s*\{([^}]*)\}/s)?.[1] || '';
assert.match(shotStyle, /overflow:\s*hidden/, 'the screenshot keeps rounded clipping');

console.log('[pass] page annotation transcript renders as a card');
