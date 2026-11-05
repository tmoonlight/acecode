import assert from 'node:assert/strict';
import { clipboardImagesAreTextSnapshot } from './clipboardTextSnapshot.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const text = (nodeValue) => ({ nodeType: 3, nodeValue });
const element = (tagName, childNodes = []) => ({ nodeType: 1, tagName, childNodes });
const comment = () => ({ nodeType: 8, childNodes: [] });

// Node 测试环境没有 DOMParser:按 HTML 原文查表返回手工构造的 body 节点树。
function parserFor(bodies) {
  return {
    parseHtml(html) {
      assert.ok(Object.hasOwn(bodies, html), `unexpected html: ${html}`);
      return { body: element('BODY', bodies[html]) };
    },
  };
}

function clipboard(data) {
  return {
    types: Object.keys(data),
    getData: (type) => data[type] || '',
  };
}

const bitmap = { name: 'image.png', type: 'image/png', size: 2048 };

run('OneNote text selection pastes as text, not its bitmap', () => {
  // 场景:客户在 OneNote 2021 里选中几行文字复制,剪贴板同时有 HTML、纯文本和一张选区截图,
  // Chromium 把截图当成 image/png 文件放进粘贴事件。
  // 期望:判定为文字截图,输入框插入文字而不是添加图片附件。
  // 回归:旧实现只要有图片文件就当附件,文字被一张截图替代。
  const html = '<html><head><style>p{margin:0}</style></head><body><p>会议纪要</p><p>第一条</p></body></html>';
  const data = clipboard({ 'text/html': html, 'text/plain': '会议纪要\r\n第一条' });
  const options = parserFor({
    [html]: [element('P', [text('会议纪要')]), element('P', [text('第一条')])],
  });
  assert.equal(clipboardImagesAreTextSnapshot(data, [bitmap], options), true);
});

run('Excel cells with RTF and HTML paste as text', () => {
  // 场景:Excel 复制单元格,剪贴板有 HTML 表格、RTF、制表符分隔的纯文本和一张截图。
  // 期望:粘贴成文字(模型能读到单元格内容),不是表格截图。
  const html = '<table><tr><td>名称</td><td>数量</td></tr></table>';
  const data = clipboard({ 'text/html': html, 'text/rtf': '{\\rtf1 ...}', 'text/plain': '名称\t数量' });
  const options = parserFor({
    [html]: [element('TABLE', [element('TR', [
      element('TD', [text('名称')]),
      element('TD', [text('数量')]),
    ])])],
  });
  assert.equal(clipboardImagesAreTextSnapshot(data, [bitmap], options), true);
});

run('browser "Copy image" keeps the image even with an image URL as plain text', () => {
  // 场景:浏览器右键「复制图片」,HTML 只有一个 <img>(带 alt);Firefox 还会把图片地址放进纯文本。
  // 期望:HTML 没有可见文字,仍然按图片附件处理,不能退化成贴一行图片地址。
  const html = '<!--StartFragment--><img src="https://example.test/cat.png" alt="猫">';
  const data = clipboard({ 'text/html': html, 'text/plain': 'https://example.test/cat.png' });
  const options = parserFor({
    [html]: [comment(), element('IMG')],
  });
  assert.equal(clipboardImagesAreTextSnapshot(data, [bitmap], options), false);
});

run('Word picture-only selection stays an image', () => {
  // 场景:Word 里只选中一张图片复制,HTML 是包着 <img> 的空段落,RTF 存在,纯文本只有空白。
  // 期望:没有可见文字,继续当图片粘贴。
  const html = '<p class=MsoNormal><img src="file:///C:/clip_image001.png"><o:p>&nbsp;</o:p></p>';
  const data = clipboard({ 'text/html': html, 'text/rtf': '{\\rtf1 ...}', 'text/plain': ' \r\n' });
  const options = parserFor({
    [html]: [element('P', [element('IMG'), element('O:P', [text('\u00a0')])])],
  });
  assert.equal(clipboardImagesAreTextSnapshot(data, [bitmap], options), false);
});

run('screenshots and image-only clipboards stay images', () => {
  // 场景:截图工具只放位图;或者只有图片加一段纯文本、没有 HTML 也没有 RTF(无法证明是 Office 文字)。
  // 期望:都按图片处理,保持原有截图粘贴行为。
  assert.equal(clipboardImagesAreTextSnapshot(clipboard({}), [bitmap]), false);
  assert.equal(clipboardImagesAreTextSnapshot(clipboard({ 'text/plain': 'C:\\shots\\a.png' }), [bitmap]), false);
});

run('Office text without HTML is recognized by RTF', () => {
  // 场景:来源只给了 RTF 和纯文本(没有 HTML)外加一张截图,RTF 是 Office 系来源的特征。
  // 期望:纯文本非空时判定为文字;纯文本为空时仍保留图片,避免什么都贴不进去。
  const withText = clipboard({ 'text/rtf': '{\\rtf1 hello}', 'text/plain': 'hello' });
  const withoutText = clipboard({ 'text/rtf': '{\\rtf1 }', 'text/plain': '  ' });
  assert.equal(clipboardImagesAreTextSnapshot(withText, [bitmap]), true);
  assert.equal(clipboardImagesAreTextSnapshot(withoutText, [bitmap]), false);
});

run('non-image files and empty file lists are never dropped', () => {
  // 场景:粘贴里带有非栅格图片的文件(比如 PDF),或者根本没有文件。
  // 期望:不判定为文字截图,文件照常走附件 / 路径流程。
  const html = '<p>说明</p>';
  const data = clipboard({ 'text/html': html, 'text/plain': '说明' });
  const options = parserFor({ [html]: [element('P', [text('说明')])] });
  const pdf = { name: 'report.pdf', type: 'application/pdf', size: 10 };
  assert.equal(clipboardImagesAreTextSnapshot(data, [bitmap, pdf], options), false);
  assert.equal(clipboardImagesAreTextSnapshot(data, [], options), false);
  assert.equal(clipboardImagesAreTextSnapshot(null, [bitmap]), false);
});
