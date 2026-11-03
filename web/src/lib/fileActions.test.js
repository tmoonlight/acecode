import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { saveFileAs, readFileActionText } from './fileActions.js';
import { buildTurnFileItems } from './turnFileList.js';
import { buildDesktopContextMenuItems, contextTargetsFromElement, DESKTOP_CONTEXT_ACTIONS as A, TURN_FILE_TARGET_SELECTOR } from './desktopContextMenu.js';

async function test(name, fn) {
  await fn();
  console.log(`[pass] ${name}`);
}

function targetFor(path, cwd) {
  const row = {
    closest: selector => selector === TURN_FILE_TARGET_SELECTOR ? row : null,
    getAttribute: name => ({ 'data-desktop-turn-file-path': path, 'data-desktop-turn-file-cwd': cwd })[name] || '',
  };
  return contextTargetsFromElement({ closest: row.closest }).turnFileTarget;
}

await test('turn cards resolve corrected worktree, external, POSIX, UNC and missing roots', () => {
  const cwd = 'N:/project/.acecode/worktrees/session';
  const [item] = buildTurnFileItems([{ file: '.acecode/worktrees/session/src/报告.cpp' }], cwd);
  assert.equal(targetFor(item.openPath, cwd).absolutePath, 'N:\\project\\.acecode\\worktrees\\session\\src\\报告.cpp');
  assert.equal(targetFor('D:/other/a.txt', cwd).absolutePath, 'D:\\other\\a.txt');
  assert.equal(targetFor('../a.txt', '/tmp/project').absolutePath, '/tmp/a.txt');
  assert.equal(targetFor('a.txt', '\\\\server\\share\\project').absolutePath, '\\\\server\\share\\project\\a.txt');
  assert.equal(targetFor('relative.txt', '').absolutePath, '');
  assert.equal(targetFor('/tmp/a.txt', '').absolutePath, '/tmp/a.txt');
});

await test('card menu has exactly four ordered actions and wins over surrounding message/link targets', () => {
  const target = targetFor('a.txt', 'N:/project');
  const items = buildDesktopContextMenuItems({ turnFileTarget: target,
    messageTarget: { role: 'assistant' }, fileLinkTarget: { path: 'other.txt' }, hasSelection: true, debug: true });
  assert.deepEqual(items.map(item => item.id), [A.SAVE_FILE_AS, A.COPY_ABSOLUTE_PATH, A.COPY_FILE_CONTENTS, A.OPEN_IN_EXPLORER]);
  assert.ok(items.every(item => item.enabled && !item.separatorBefore));
  assert.equal(items[1].label, '复制路径');
  assert.equal(items[3].label, '在资源管理器中打开');
  assert.equal(items[3].target.path, target.absolutePath);
  assert.equal(buildDesktopContextMenuItems({ turnFileTarget: target, allowNativeActions: false }).length, 3);
  assert.ok(buildDesktopContextMenuItems({ turnFileTarget: targetFor('a.txt', '') }).every(item => !item.enabled));
  assert.ok(!buildDesktopContextMenuItems({ turnFileTarget: target, editable: true }).some(item => item.id === A.SAVE_FILE_AS));
});

await test('native Save as preserves cancellation and never downloads', async () => {
  const target = targetFor('src/a.cpp', 'N:/project');
  const result = await saveFileAs(target, { native: true, client: {
    exportFile: async (cwd, path) => {
      assert.equal(cwd, 'N:/project');
      assert.equal(path, 'N:\\project\\src\\a.cpp');
      return { ok: true, cancelled: true };
    },
  }, download: () => assert.fail('native export must not download') });
  assert.deepEqual(result, { ok: true, cancelled: true });
});

await test('browser Save as downloads original binary bytes with the basename', async () => {
  const bytes = new Blob([new Uint8Array([0, 255, 13, 10, 128])]);
  const result = await saveFileAs(targetFor('图像.bin', '/tmp'), { client: {
    readFileBlob: async (cwd, path, options) => {
      assert.equal(cwd, '/tmp');
      assert.equal(path, '/tmp/图像.bin');
      assert.deepEqual(options, { download: true });
      return bytes;
    },
  }, download: (blob, name) => {
    assert.equal(blob, bytes);
    assert.equal(name, '图像.bin');
    return { ok: true, filename: name };
  } });
  assert.equal(result.downloaded, true);
});

await test('content actions read current full text including empty text; failures never download', async () => {
  const target = targetFor('/tmp/empty.txt', '');
  assert.equal(await readFileActionText(target, { client: {
    readFile: async (cwd, path) => { assert.equal(cwd, '/tmp/'); assert.equal(path, '/tmp/empty.txt'); return ''; },
  } }), '');
  for (const message of ['binary', 'not found', 'file too large']) {
    await assert.rejects(readFileActionText(target, { client: { readFile: async () => { throw new Error(message); } } }), { message });
  }
  await assert.rejects(saveFileAs(target, { client: {
    readFileBlob: async () => { throw new Error('not found'); },
  }, download: () => assert.fail('failed reads must not download') }), /not found/);
});

await test('card marker is on the full row and retains independent file/diff buttons', () => {
  const source = readFileSync(new URL('../components/TurnFileList.jsx', import.meta.url), 'utf8');
  assert.match(source, /className="ace-turn-file-row"\s+data-desktop-turn-file-path=\{item.openPath\}\s+data-desktop-turn-file-cwd=\{cwd\}/);
  assert.match(source, /onOpenFile\?\.\(item.openPath\)/);
  assert.match(source, /onOpenChanges\?\.\(item.file, turnUserMessageId\)/);
});
