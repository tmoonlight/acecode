import assert from 'node:assert/strict';
import { htmlPreviewFileUrl } from './htmlPreview.js';

assert.equal(htmlPreviewFileUrl({ cwd: 'N:\\work tree', path: '页面 #100%.HTML' }),
  'file:///N:/work%20tree/%E9%A1%B5%E9%9D%A2%20%23100%25.HTML');
assert.equal(htmlPreviewFileUrl({ cwd: 'C:/other', path: 'D:\\pages\\index.HtM' }),
  'file:///D:/pages/index.HtM');
assert.equal(htmlPreviewFileUrl({ cwd: '/Users/me/worktree', path: '../page?.html' }),
  'file:///Users/me/page%3F.html');
assert.equal(htmlPreviewFileUrl({ cwd: '\\\\server\\share', path: 'page one.htm' }),
  'file://server/share/page%20one.htm');
assert.equal(htmlPreviewFileUrl({ path: '\\\\?\\C:\\pages\\a.html' }), 'file:///C:/pages/a.html');
assert.equal(htmlPreviewFileUrl({ path: '/tmp/a%20b.html' }), 'file:///tmp/a%2520b.html');
for (const path of ['a.txt', 'a.md', 'a.xhtml', 'page.html.js', 'html', '']) {
  assert.equal(htmlPreviewFileUrl({ cwd: '/tmp', path }), '');
}
assert.equal(htmlPreviewFileUrl({ path: 'index.html' }), '');
assert.equal(htmlPreviewFileUrl({ cwd: 'relative', path: 'index.html' }), '');
assert.equal(htmlPreviewFileUrl(), '');
console.log('[pass] HTML preview URLs resolve local roots and encode path characters');
