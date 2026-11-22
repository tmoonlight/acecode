import assert from 'node:assert/strict';
import fs from 'node:fs';
import { formatAnnotationBatch } from './pageAnnotationBatches.js';
import {
  contentPartsWithoutAnnotationScreenshot,
  pageAnnotationMessageCopyText,
  pageAnnotationScreenshot,
  parsePageAnnotationMessage,
} from './pageAnnotationMessage.js';

const text = formatAnnotationBatch([{
  number: 3,
  description: '这里文字加粗',
  targets: [{ id: 'target-10', tag: 'p', locator: 'p', text: '整段元素原文', bounds: { x: 20, y: 59, width: 560, height: 93 } }],
  region: null,
  differences: [{ property: 'font-weight', previous: '400', next: '700' }],
}, {
  number: 4,
  description: '   ',
  targets: [],
  region: null,
  differences: [],
}], { url: 'http://127.0.0.1:8765/moon-garden.html', title: '月面温室档案 / 2049' });

const card = parsePageAnnotationMessage(text);
assert.equal(card.title, '月面温室档案 / 2049');
assert.deepEqual(card.comments, [{ number: 3, description: '这里文字加粗' }]);
assert.equal(pageAnnotationMessageCopyText(card), '月面温室档案 / 2049\n这里文字加粗');
assert.equal(JSON.stringify(card).includes('整段元素原文'), false);
assert.equal(JSON.stringify(card).includes('font-weight'), false);
assert.equal(parsePageAnnotationMessage('Page annotations\nnot-json'), null);
assert.equal(parsePageAnnotationMessage('普通消息'), null);

const several = parsePageAnnotationMessage(formatAnnotationBatch([
  { number: 1, description: '第一处', targets: [], region: null, differences: [] },
  { number: 2, description: '第二处', targets: [], region: null, differences: [] },
], { title: '页面' }));
assert.equal(pageAnnotationMessageCopyText(several), '页面\n1 第一处\n2 第二处');

const parts = [
  { type: 'image', attachment: { id: 'shot', name: 'page-annotations-batch-1.png', mime_type: 'image/png', blob_url: '/shot.png' } },
  { type: 'image', attachment: { id: 'other', name: 'photo.png', mime_type: 'image/png', blob_url: '/photo.png' } },
];
const screenshot = pageAnnotationScreenshot(parts);
assert.equal(screenshot.id, 'shot');
assert.deepEqual(contentPartsWithoutAnnotationScreenshot(parts, screenshot).map((part) => part.attachment.id), ['other']);

const panel = fs.readFileSync(new URL('../components/AgentBrowserPanel.jsx', import.meta.url), 'utf8');
assert.equal(panel.includes('<span>批注</span>'), false);
const message = fs.readFileSync(new URL('../components/Message.jsx', import.meta.url), 'utf8');
assert.match(message, /parsePageAnnotationMessage/);
assert.match(message, /PageAnnotationMessageCard/);
assert.match(message, /contentPartsWithoutAnnotationScreenshot/);
const cardComponent = fs.readFileSync(new URL('../components/PageAnnotationMessageCard.jsx', import.meta.url), 'utf8');
assert.match(cardComponent, /条注释/);

console.log('[pass] page annotation transcript renders as a card');
