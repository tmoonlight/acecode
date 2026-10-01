import assert from 'node:assert/strict';
import { resolveComposerFileIntake } from './composerFileIntake.js';
import { fileSourcePath, markFileSourcePath } from './composerFileTransfer.js';

const item = path => ({ kind: 'file', path, name: path.split(/[\\/]/).pop(), reference_only: true, size_bytes: 1 });
const win = {
  __ACECODE_DESKTOP_SHELL__: true,
  __ACECODE_OS__: 'windows',
  aceDesktop_materializeContextItems: async paths => ({ ok: true, items: paths.map(item) }),
};
const file = new File(['file'], 'notes.txt');
for (const source of ['drop', 'paste', 'picker']) {
  const result = await resolveComposerFileIntake({ source, files: [file] }, {});
  assert.deepEqual(result, { kind: 'upload', files: [file] });
}
console.log('[pass] browser file intake uses the same upload contract for all entries');

const copied = { ...win, aceDesktop_readClipboardContextItems: async () => ({ ok: true, items: [item('C:/work/notes.txt')] }) };
assert.deepEqual(
  await resolveComposerFileIntake({ source: 'paste', files: [file] }, copied),
  await resolveComposerFileIntake({ source: 'drop', paths: ['C:/work/notes.txt'], files: [file] }, win),
);
assert.equal((await resolveComposerFileIntake({ source: 'paste', files: [file], uriList: 'file:///C:/work/notes.txt' }, win)).items[0].path, 'C:\\work\\notes.txt');
for (const os of ['linux', 'macos']) {
  const resolved = await resolveComposerFileIntake({ source: 'paste', uriList: 'file:///home/me/%E4%B8%AD%E6%96%87%20notes.txt' }, { ...win, __ACECODE_OS__: os });
  assert.equal(resolved.items[0].path, '/home/me/中文 notes.txt');
}
assert.equal((await resolveComposerFileIntake({ source: 'paste', uriList: 'https://example.com/file' }, win)).kind, 'none');
console.log('[pass] native drop and paste keep identical references and handle Windows and POSIX paths');

let saves = 0;
const saveHost = {
  ...win,
  aceDesktop_readClipboardContextItems: async () => ({ ok: true, items: [], filesystem_items: false }),
  aceDesktop_storeContextFiles: async files => {
    saves++;
    assert.equal(atob(files[0].data_base64), 'file');
    return { ok: true, items: files.map(file => item('C:/local-cache/' + file.name)) };
  },
};
for (const source of ['drop', 'paste']) {
  const resolved = await resolveComposerFileIntake({ source, files: [file] }, saveHost);
  assert.equal(resolved.kind, 'paths');
  assert.equal(resolved.items[0].path, 'C:/local-cache/notes.txt');
}
assert.equal(saves, 2);
const known = markFileSourcePath(new File(['file'], 'known.png'), 'C:/source/known.png');
assert.equal((await resolveComposerFileIntake({ source: 'drop', files: [known] }, saveHost)).items[0].path, 'C:/source/known.png');
assert.equal(saves, 2, 'known paths must never be re-saved');
await assert.rejects(resolveComposerFileIntake({ source: 'paste', files: [file] }, {
  ...saveHost, aceDesktop_readClipboardContextItems: async () => ({ ok: false, error: 'clipboard locked' }),
}), /clipboard locked/);
assert.equal(saves, 2, 'clipboard errors must not become file data fallbacks');
await assert.rejects(resolveComposerFileIntake({ source: 'drop', files: [file] }, win), /更新客户端/);
assert.equal((await resolveComposerFileIntake({ source: 'paste' }, saveHost)).kind, 'none');
console.log('[pass] desktop pathless data stays local; clipboard failures never silently upload or save');

// ---- 栅格图片走快照附件(缩略图 + 模型直接可见),不再变成 @路径 ----

const png = (name = 'shot.png') => new File(['png-bytes'], name, { type: 'image/png' });
const imageItem = (path, data = 'png-bytes') => ({
  kind: 'file', path, name: path.split(/[\\/]/).pop(), mime_type: 'image/png', size_bytes: data.length, data_base64: btoa(data),
});

// 触发场景:Desktop 里粘贴截图(剪贴板只有位图,没有文件系统条目)。
// 期望行为:图片数据直接作为附件上传(输入框显示缩略图),不落盘成 composer-files 再插 @路径。
// 回归:曾经截图被存成 ~/.acecode/composer-files/<uuid>/image.png 并以 @路径 插入,
// 输入框与对话记录只剩文件名,模型还得先 bash 再 show_image 才看得到图。
{
  let stored = 0;
  const host = { ...saveHost, aceDesktop_storeContextFiles: async () => { stored++; return { ok: true, items: [] }; } };
  const shot = png('image.png');
  assert.deepEqual(await resolveComposerFileIntake({ source: 'paste', files: [shot] }, host), { kind: 'upload', files: [shot] });
  assert.equal(stored, 0, 'pathless image data must not be saved as a path reference');
}
console.log('[pass] desktop screenshot paste uploads the image instead of saving a path reference');

// 触发场景:Desktop 拖入 / 选择 / 从资源管理器复制一个本地图片文件,原生层读出了字节(data_base64)。
// 期望行为:变成带来源路径标记的 File 走附件上传;同一手势里的文件夹与普通文件仍是路径引用,顺序保持。
{
  const host = {
    ...win,
    aceDesktop_materializeContextItems: async (paths) => ({ ok: true, items: paths.map((path) => (
      path.endsWith('.png') ? imageItem(path) : item(path)
    )) }),
  };
  const dropped = await resolveComposerFileIntake({ source: 'drop', paths: ['C:/pics/shot.png', 'C:/work/notes.txt'] }, host);
  assert.equal(dropped.kind, 'paths');
  assert.deepEqual(dropped.items.map((entry) => entry.path), ['C:/work/notes.txt']);
  assert.equal(dropped.files.length, 1);
  assert.equal(dropped.files[0].name, 'shot.png');
  assert.equal(dropped.files[0].type, 'image/png');
  assert.equal(await dropped.files[0].text(), 'png-bytes');
  assert.equal(fileSourcePath(dropped.files[0]), 'C:/pics/shot.png');

  const picked = await resolveComposerFileIntake({ source: 'picker', items: [imageItem('C:/pics/a.png')] }, host);
  assert.equal(picked.kind, 'upload');
  assert.equal(fileSourcePath(picked.files[0]), 'C:/pics/a.png');
}
console.log('[pass] desktop local raster images with native bytes become snapshot uploads beside path references');

// 触发场景:原生层没有给图片字节 —— 超过 25 MiB、读取失败,或旧版桌面壳一律只给路径;以及 SVG。
// 期望行为:退回路径引用,添加本身不失败。
{
  const reference = { ...item('C:/pics/huge.png'), mime_type: 'image/png' };
  const svg = { kind: 'file', path: 'C:/pics/logo.svg', name: 'logo.svg', mime_type: 'image/svg+xml', size_bytes: 3, data_base64: btoa('svg') };
  const result = await resolveComposerFileIntake({ source: 'picker', items: [reference, svg] }, win);
  assert.deepEqual(result, { kind: 'paths', items: [reference, svg] });
}
console.log('[pass] images without native bytes and SVG stay path references');

// 触发场景:Desktop 里拖入 / 粘贴的 File 本身带图片字节(有无可信来源路径都一样)。
// 期望行为:图片直接上传,不经原生落盘或路径化;普通文件仍按原有规则走路径。
{
  let saves = 0;
  const host = { ...saveHost, aceDesktop_storeContextFiles: async (files) => {
    saves++;
    return { ok: true, items: files.map((entry) => item('C:/local-cache/' + entry.name)) };
  } };
  const known = markFileSourcePath(png('known.png'), 'C:/source/known.png');
  const result = await resolveComposerFileIntake({ source: 'drop', files: [known, file] }, host);
  assert.equal(result.kind, 'paths');
  assert.deepEqual(result.items.map((entry) => entry.path), ['C:/local-cache/notes.txt']);
  assert.deepEqual(result.files, [known]);
  assert.equal(fileSourcePath(result.files[0]), 'C:/source/known.png');
  assert.equal(saves, 1, 'only the ordinary file is saved locally');
}
console.log('[pass] desktop image Files upload directly while ordinary files keep the path contract');
