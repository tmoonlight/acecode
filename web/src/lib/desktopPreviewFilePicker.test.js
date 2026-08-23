import assert from 'node:assert/strict';
import {
  hasNativePreviewFilePicker,
  parseNativePreviewFilePickerResult,
  pickNativePreviewFile,
} from './desktopPreviewFilePicker.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('preview file picker detects only its dedicated desktop bridge', () => {
  assert.equal(hasNativePreviewFilePicker({ aceDesktop_pickPreviewFile() {} }), true);
  assert.equal(hasNativePreviewFilePicker({ aceDesktop_pickContextItems() {} }), false);
  assert.equal(hasNativePreviewFilePicker({}), false);
});

await run('preview file picker accepts one absolute file path', () => {
  assert.deepEqual(parseNativePreviewFilePickerResult(JSON.stringify({
    ok: true,
    cancelled: false,
    path: 'C:\\repo\\src\\main.cpp',
  })), {
    cancelled: false,
    path: 'C:\\repo\\src\\main.cpp',
  });
  assert.deepEqual(parseNativePreviewFilePickerResult({
    ok: true,
    path: '/tmp/main.cpp',
  }), {
    cancelled: false,
    path: '/tmp/main.cpp',
  });
});

await run('preview file picker treats cancellation as a quiet empty result', () => {
  assert.deepEqual(parseNativePreviewFilePickerResult({
    ok: true,
    cancelled: true,
  }), {
    cancelled: true,
    path: '',
  });
});

await run('preview file picker rejects bridge failures and non-absolute paths', () => {
  assert.throws(
    () => parseNativePreviewFilePickerResult({ ok: false, error: 'picker failed' }),
    /picker failed/,
  );
  assert.throws(
    () => parseNativePreviewFilePickerResult({ ok: true, path: 'src/main.cpp' }),
    /无法获取文件路径/,
  );
  assert.throws(
    () => parseNativePreviewFilePickerResult(''),
    /原生选择器返回无效结果/,
  );
});

await run('preview file picker forwards the current workspace as the default directory', async () => {
  const calls = [];
  const result = await pickNativePreviewFile('N:\\work\\acecode', {
    async aceDesktop_pickPreviewFile(payload) {
      calls.push(payload);
      return JSON.stringify({
        ok: true,
        cancelled: false,
        path: 'N:\\work\\acecode\\README.md',
      });
    },
  });
  assert.deepEqual(calls, [{ cwd: 'N:\\work\\acecode' }]);
  assert.deepEqual(result, {
    cancelled: false,
    path: 'N:\\work\\acecode\\README.md',
  });
});

await run('preview file picker refuses to run without the native bridge', async () => {
  await assert.rejects(
    () => pickNativePreviewFile('N:\\work\\acecode', {}),
    /原生选择器不可用/,
  );
});
