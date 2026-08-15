import assert from 'node:assert/strict';
import {
  composerFileIdentity,
  fileSourcePath,
  fileSourceReference,
  filesFromClipboardEvent,
  filesFromTransfer,
  hasFileTransfer,
  markFileSourcePath,
  markFileSourceReference,
} from './composerFileTransfer.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function fileLike(name, type = 'application/octet-stream') {
  return { name, type, size: 1 };
}

function list(items) {
  const out = { length: items.length, item: (index) => items[index] || null };
  items.forEach((item, index) => {
    out[index] = item;
  });
  return out;
}

run('drop transfer reads external files from FileList', () => {
  const png = fileLike('shot.png', 'image/png');
  const txt = fileLike('notes.txt', 'text/plain');
  const transfer = { files: list([png, txt]), items: list([]) };

  assert.equal(hasFileTransfer(transfer), true);
  assert.deepEqual(filesFromTransfer(transfer), [png, txt]);
});

run('item list fallback extracts clipboard image files', () => {
  const png = fileLike('clipboard.png', 'image/png');
  const transfer = {
    files: list([]),
    items: list([
      { kind: 'string', type: 'text/plain', getAsFile: () => null },
      { kind: 'file', type: 'image/png', getAsFile: () => png },
    ]),
  };

  assert.equal(hasFileTransfer(transfer), true);
  assert.deepEqual(filesFromTransfer(transfer, { source: 'paste' }), [png]);
});

run('text-only clipboard paste is ignored', () => {
  const event = {
    clipboardData: {
      files: list([]),
      items: list([{ kind: 'string', type: 'text/plain' }]),
    },
  };

  assert.equal(hasFileTransfer(event.clipboardData), false);
  assert.deepEqual(filesFromClipboardEvent(event), []);
});

run('unnamed pasted images get a stable filename when File is available', () => {
  if (typeof File !== 'function' || typeof Blob !== 'function') return;

  const unnamed = new File([new Blob(['x'], { type: 'image/png' })], '', { type: 'image/png' });
  const event = {
    clipboardData: {
      files: list([]),
      items: list([{ kind: 'file', type: 'image/png', getAsFile: () => unnamed }]),
    },
  };

  const files = filesFromClipboardEvent(event);
  assert.equal(files.length, 1);
  assert.equal(files[0].name, 'pasted-image.png');
  assert.equal(files[0].type, 'image/png');
});

run('native source paths are preserved separately from the browser filename', () => {
  const file = fileLike('notes.txt', 'text/plain');
  assert.equal(markFileSourcePath(file, 'C:\\work\\project\\notes.txt'), file);
  assert.equal(fileSourcePath(file), 'C:/work/project/notes.txt');
  assert.equal(Object.keys(file).includes('acecodeSourcePath'), false);
  assert.equal(fileSourcePath(markFileSourcePath(fileLike('x'), 'relative/x')), '');
});

run('native source references preserve actual size without becoming file bytes', () => {
  const file = fileLike('large.pdf', 'application/pdf');
  markFileSourceReference(file, 'C:\\work\\large.pdf', 50 * 1024 * 1024);

  assert.deepEqual(fileSourceReference(file), {
    sourcePath: 'C:/work/large.pdf',
    sizeBytes: 50 * 1024 * 1024,
  });
  assert.equal(fileSourcePath(file), 'C:/work/large.pdf');
  assert.equal(fileSourceReference(markFileSourceReference(
    fileLike('bad.txt'), 'relative/bad.txt', 1,
  )), null);
});

run('composer file identity normalizes Windows path separators and casing', () => {
  const first = markFileSourcePath(fileLike('notes.txt', 'text/plain'), 'C:\\Work\\Project\\Notes.txt');
  const second = markFileSourcePath(fileLike('renamed.txt', 'application/octet-stream'), 'c:/work/project/notes.TXT');
  const uncFirst = markFileSourcePath(fileLike('asset.bin'), '\\\\Server\\Share\\Asset.bin');
  const uncSecond = markFileSourcePath(fileLike('asset-copy.bin'), '//server/share/asset.BIN');

  assert.equal(composerFileIdentity(first), composerFileIdentity(second));
  assert.equal(composerFileIdentity(uncFirst), composerFileIdentity(uncSecond));
  assert.match(composerFileIdentity(first), /^path:/);
});

run('composer file identity keeps POSIX source path casing distinct', () => {
  const first = markFileSourcePath(fileLike('notes.txt'), '/work/Notes.txt');
  const second = markFileSourcePath(fileLike('notes.txt'), '/work/notes.txt');

  assert.notEqual(composerFileIdentity(first), composerFileIdentity(second));
});

run('composer file identity falls back to stable browser metadata', () => {
  const first = {
    name: 'report.txt',
    type: 'TEXT/PLAIN',
    size: 42,
    lastModified: 123456,
  };
  const same = {
    name: 'report.txt',
    type: 'text/plain',
    size: 42,
    lastModified: 123456,
  };
  const changed = { ...same, lastModified: 123457 };

  assert.equal(composerFileIdentity(first), composerFileIdentity(same));
  assert.notEqual(composerFileIdentity(first), composerFileIdentity(changed));
  assert.match(composerFileIdentity(first), /^file:/);
});
