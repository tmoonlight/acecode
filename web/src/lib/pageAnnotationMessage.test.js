import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  annotationShotsByName,
  contentPartsWithoutAnnotationScreenshots,
  pageAnnotationCardFromContexts,
  pageAnnotationMessageCopyText,
  pageAnnotationScreenshot,
} from './pageAnnotationMessage.js';

const parts = [
  { type: 'image', attachment: { id: 'shot', name: 'page-annotations-c1-1.png', mime_type: 'image/png', blob_url: '/shot.png' } },
  { type: 'image', attachment: { id: 'other', name: 'photo.png', mime_type: 'image/png', blob_url: '/photo.png' } },
];

// 批注只存在于上下文部件，正文保持自然语言。
const contextParts = [{
  type: 'page_annotations_context',
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
assert.deepEqual(contextual.pages.map((page) => page.title), ['商品详情页']);
assert.deepEqual(contextual.comments.map((comment) => comment.description), ['卡片间距太小']);
assert.equal(annotationShotsByName(contextParts).get('page-annotations-c1-1.png').id, 'shot');
assert.equal(pageAnnotationMessageCopyText(contextual, '只显示自然语言正文'),
  '只显示自然语言正文\n\ndiv 卡片间距太小');
assert.equal(pageAnnotationCardFromContexts(parts), null);

assert.equal(pageAnnotationScreenshot(parts).id, 'shot');
assert.deepEqual([...annotationShotsByName(parts).keys()], ['page-annotations-c1-1.png']);
assert.deepEqual(contentPartsWithoutAnnotationScreenshots(parts).map((part) => part.attachment.id), ['other']);

const message = fs.readFileSync(new URL('../components/Message.jsx', import.meta.url), 'utf8');
assert.doesNotMatch(message, /parsePageAnnotationMessage/);
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
