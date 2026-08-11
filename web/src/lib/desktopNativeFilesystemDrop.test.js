import assert from 'node:assert/strict';
import {
  WINDOWS_NATIVE_FILESYSTEM_DROP_MESSAGE,
  nativeFilesystemDropObjects,
  postWindowsNativeFilesystemDrop,
} from './desktopNativeFilesystemDrop.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function list(items) {
  const out = { length: items.length, item: (index) => items[index] || null };
  items.forEach((item, index) => { out[index] = item; });
  return out;
}

run('posts an ordered multi-file drop as one WebView2 message', () => {
  const first = { name: 'first.txt' };
  const second = { name: 'second.png' };
  const calls = [];
  const webview = {
    postMessageWithAdditionalObjects(message, objects) {
      calls.push({ receiver: this, message, objects });
    },
  };
  const transfer = {
    items: list([
      { kind: 'file', getAsFile: () => first },
      { kind: 'file', getAsFile: () => second },
    ]),
    files: list([]),
  };

  assert.equal(postWindowsNativeFilesystemDrop(transfer, { chrome: { webview } }), true);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].receiver, webview);
  assert.equal(calls[0].message, WINDOWS_NATIVE_FILESYSTEM_DROP_MESSAGE);
  assert.deepEqual(calls[0].objects, [first, second]);
});

run('keeps a file-kind folder object in a mixed transfer', () => {
  const file = { name: 'notes.txt', size: 12 };
  const folder = { name: 'docs', size: 0 };
  const transfer = {
    items: list([
      { kind: 'file', getAsFile: () => file },
      { kind: 'file', getAsFile: () => folder },
    ]),
    files: list([file]),
  };

  assert.deepEqual(nativeFilesystemDropObjects(transfer), [file, folder]);
});

run('falls back to FileList when transfer items yield no files', () => {
  const file = { name: 'fallback.txt' };
  const transfer = {
    items: list([
      { kind: 'string', getAsFile: () => null },
      { kind: 'file', getAsFile: () => null },
    ]),
    files: list([file]),
  };

  assert.deepEqual(nativeFilesystemDropObjects(transfer), [file]);
});

run('does not claim a drop when the bridge or objects are unavailable', () => {
  const file = { name: 'x.txt' };
  const transfer = { items: list([]), files: list([file]) };
  assert.equal(postWindowsNativeFilesystemDrop(transfer, {}), false);
  assert.equal(postWindowsNativeFilesystemDrop(
    { items: list([]), files: list([]) },
    { chrome: { webview: { postMessageWithAdditionalObjects() {} } } },
  ), false);
});

run('reports bridge failure without throwing', () => {
  const transfer = { items: list([]), files: list([{ name: 'x.txt' }]) };
  const win = {
    chrome: {
      webview: {
        postMessageWithAdditionalObjects() { throw new Error('unsupported'); },
      },
    },
  };
  assert.equal(postWindowsNativeFilesystemDrop(transfer, win), false);
});
