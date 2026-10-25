import assert from 'node:assert/strict';
import {
  DEFAULT_IMAGE_COMPRESSION_ENABLED,
  imageCompressionEnabled,
  imageCompressionUpdate,
} from './imageCompression.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:设置页打开时读 GET /api/config/image-compression。
// 期望行为:按响应里的布尔值显示;读不到、响应不是对象、enabled 不是布尔时按 daemon
// 默认值(开启)显示 —— 与 config 缺省时的实际行为一致,不会显示成关闭却在压缩。
run('按 daemon 的响应推出「压缩图片」开关状态', () => {
  assert.equal(DEFAULT_IMAGE_COMPRESSION_ENABLED, true);
  assert.equal(imageCompressionEnabled({ enabled: true }), true);
  assert.equal(imageCompressionEnabled({ enabled: false }), false);
  for (const value of [null, undefined, 'false', 0, [], {}, { enabled: 'false' }, { enabled: 0 }]) {
    assert.equal(imageCompressionEnabled(value), true, JSON.stringify(value));
  }
});

// 触发场景:用户点开关。
// 期望行为:布尔值生成 {enabled} 的 PUT body;其它取值返回 null,组件据此不发请求。
run('切换「压缩图片」时的 PUT body', () => {
  assert.deepEqual(imageCompressionUpdate(true), { enabled: true });
  assert.deepEqual(imageCompressionUpdate(false), { enabled: false });
  for (const value of [null, undefined, 'true', 1, {}]) {
    assert.equal(imageCompressionUpdate(value), null, String(value));
  }
});
