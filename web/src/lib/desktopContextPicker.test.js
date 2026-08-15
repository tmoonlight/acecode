import assert from 'node:assert/strict';
import {
  folderReferencePath,
  hasNativeContextPicker,
  nativeFolderReferencePath,
  nativePickedFileToFile,
  parseNativeContextPickerResult,
  parseNativeFilesystemItemsResult,
} from './desktopContextPicker.js';
import {
  fileSourcePath,
  fileSourceReference,
} from './composerFileTransfer.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

test('detects the Desktop unified context picker bridge', () => {
  assert.equal(hasNativeContextPicker({ aceDesktop_pickContextItems() {} }), true);
  assert.equal(hasNativeContextPicker({}), false);
});

test('parses file, folder, and cancelled native results', () => {
  assert.deepEqual(
    parseNativeContextPickerResult(JSON.stringify({
      ok: true,
      items: [{ kind: 'folder', path: 'C:/repo/docs' }],
    })),
    { cancelled: false, files: [], folder: { kind: 'folder', path: 'C:/repo/docs' } },
  );
  assert.equal(parseNativeContextPickerResult({ ok: true, cancelled: true, items: [] }).cancelled, true);
  assert.equal(parseNativeContextPickerResult({
    ok: true,
    items: [{ kind: 'file', path: 'C:/repo/a.txt', name: 'a.txt', data_base64: 'YQ==' }],
  }).files.length, 1);
  assert.equal(parseNativeContextPickerResult({
    ok: true,
    items: [{
      kind: 'file',
      path: 'C:/repo/large.pdf',
      name: 'large.pdf',
      size_bytes: 50 * 1024 * 1024,
      reference_only: true,
    }],
  }).files.length, 1);
});

test('rejects mixed native file and folder results', () => {
  assert.throws(() => parseNativeContextPickerResult({
    ok: true,
    items: [
      { kind: 'folder', path: 'C:/repo/docs' },
      { kind: 'file', path: 'C:/repo/a.txt', name: 'a.txt', data_base64: 'YQ==' },
    ],
  }), /冲突/);
});

test('restores native base64 bytes as a File-compatible object', () => {
  class FakeFile {
    constructor(parts, name, options) {
      this.parts = parts;
      this.name = name;
      this.type = options.type;
    }
  }
  const file = nativePickedFileToFile({
    kind: 'file',
    path: 'C:/repo/图.png',
    name: '图.png',
    mime_type: 'image/png',
    data_base64: 'AP8Q',
  }, {
    FileCtor: FakeFile,
    decodeBase64: (value) => Buffer.from(value, 'base64').toString('latin1'),
  });
  assert.equal(file.name, '图.png');
  assert.equal(file.type, 'image/png');
  assert.equal(fileSourcePath(file), 'C:/repo/图.png');
  assert.deepEqual([...file.parts[0]], [0, 255, 16]);
});

test('keeps native ordinary files as path-only references without decoding bytes', () => {
  const file = nativePickedFileToFile({
    kind: 'file',
    path: 'C:/repo/large.pdf',
    name: 'large.pdf',
    mime_type: 'application/pdf',
    size_bytes: 50 * 1024 * 1024,
    reference_only: true,
  }, {
    FileCtor: undefined,
    decodeBase64: undefined,
  });

  assert.equal(file.name, 'large.pdf');
  assert.equal(file.type, 'application/pdf');
  assert.equal(file.size, 50 * 1024 * 1024);
  assert.equal(fileSourcePath(file), 'C:/repo/large.pdf');
  assert.deepEqual(fileSourceReference(file), {
    sourcePath: 'C:/repo/large.pdf',
    sizeBytes: 50 * 1024 * 1024,
  });
});

test('parses mixed native filesystem files and folders in transfer order', () => {
  const parsed = parseNativeFilesystemItemsResult(JSON.stringify({
    ok: true,
    filesystem_items: true,
    items: [
      { kind: 'folder', path: 'C:/repo/docs', name: 'docs' },
      {
        kind: 'file',
        path: 'C:/repo/a.txt',
        name: 'a.txt',
        size_bytes: 1,
        reference_only: true,
      },
    ],
  }));
  assert.deepEqual(parsed.items.map((item) => item.kind), ['folder', 'file']);
  assert.equal(parsed.folders.length, 1);
  assert.equal(parsed.files.length, 1);
  assert.equal(parsed.filesystemItems, true);
});

test('rejects malformed native filesystem items instead of silently dropping them', () => {
  assert.throws(() => parseNativeFilesystemItemsResult({
    ok: true,
    items: [{ kind: 'file', name: 'missing-path.txt', data_base64: '' }],
  }), /条目无效/);
  assert.throws(() => parseNativeFilesystemItemsResult({
    ok: true,
    items: [{
      kind: 'file',
      path: 'C:/repo/a.txt',
      name: 'a.txt',
      reference_only: true,
      data_base64: 'YQ==',
      size_bytes: 1,
    }],
  }), /条目无效/);
});

test('converts an in-cwd native folder to a relative reference path', () => {
  assert.equal(folderReferencePath('C:\\Repo', 'c:/repo/docs/设计 文档'), 'docs/设计 文档');
  assert.equal(folderReferencePath('C:/repo', 'C:/repo'), '');
  assert.equal(folderReferencePath('/home/me/repo', '/home/me/repo/docs'), 'docs');
});

test('keeps cwd-external and cwd-less folders as absolute references', () => {
  assert.equal(folderReferencePath('C:/repo', 'D:/shared/设计 文档'), 'D:/shared/设计 文档');
  assert.equal(folderReferencePath('C:/repo', 'C:/repo-other/docs'), 'C:/repo-other/docs');
  assert.equal(folderReferencePath('', 'C:/shared/docs'), 'C:/shared/docs');
  assert.equal(folderReferencePath('/repo', '/opt/shared/docs'), '/opt/shared/docs');
});

test('prefers the bridge canonical relative folder path', () => {
  assert.equal(nativeFolderReferencePath('N:/alias/repo', {
    path: 'C:/real/repo/docs',
    relative_path: 'docs',
  }), 'docs');
  assert.throws(() => nativeFolderReferencePath('C:/repo', {
    path: 'C:/outside',
    relative_path: '../outside',
  }), /相对路径无效/);
});

test('native folder references allow cwd prefix collisions and outside folders', () => {
  assert.equal(nativeFolderReferencePath('C:/repo', {
    path: 'C:/repo-other/docs',
  }), 'C:/repo-other/docs');
  assert.equal(nativeFolderReferencePath('/repo', {
    path: '/outside',
  }), '/outside');
});
