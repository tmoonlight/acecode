import assert from 'node:assert/strict';
import {
  insertAbsoluteFolderReferences,
  insertAbsolutePathReferences,
  localPathsFromUriList,
  materializeNativeFilesystemPaths,
  readNativeClipboardFilesystemItems,
} from './desktopFilesystemTransfer.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('normalizes dropped file URIs into host-local absolute paths', async () => {
  assert.deepEqual(localPathsFromUriList(
    'file:///C:/work/a%20b.txt\r\nfile://server/share/docs\r\n',
    'windows',
  ), ['C:\\work\\a b.txt', '\\\\server\\share\\docs']);
});

await run('materializer parses native file and folder payloads', async () => {
  const result = await materializeNativeFilesystemPaths(['C:/repo/a.txt'], {
    aceDesktop_materializeContextItems: async (paths) => JSON.stringify({
      ok: true,
      items: [
        {
          kind: 'file',
          path: paths[0],
          name: 'a.txt',
          size_bytes: 1,
          reference_only: true,
        },
        { kind: 'folder', path: 'C:/repo/docs', name: 'docs' },
      ],
    }),
  });
  assert.deepEqual(result.items.map((item) => item.kind), ['file', 'folder']);
});

await run('clipboard unavailable result remains an empty browser fallback', async () => {
  const result = await readNativeClipboardFilesystemItems({
    aceDesktop_readClipboardContextItems: async () => ({
      ok: true,
      filesystem_items: false,
      items: [],
    }),
  });
  assert.equal(result.filesystemItems, false);
  assert.deepEqual(result.items, []);
});

await run('folder references preserve absolute paths and transfer order', async () => {
  const result = insertAbsoluteFolderReferences('inspect ', 8, [
    { kind: 'folder', path: 'C:/work/one' },
    { kind: 'folder', path: 'C:/work/two words' },
  ]);
  assert.equal(result.text, 'inspect @C:/work/one/ @"C:/work/two words/" ');
  assert.equal(result.cursor, result.text.length);
});

await run('mixed native files and folders become ordered path references', async () => {
  const result = insertAbsolutePathReferences('inspect ', 8, [
    { kind: 'file', path: 'C:/work/report.pdf' },
    { kind: 'folder', path: 'C:/work/design docs' },
    { kind: 'file', path: 'C:/work/final report.zip' },
  ]);
  assert.equal(
    result.text,
    'inspect @C:/work/report.pdf @"C:/work/design docs/" @"C:/work/final report.zip" ',
  );
  assert.equal(result.cursor, result.text.length);
});
