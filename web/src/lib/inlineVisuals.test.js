import assert from 'node:assert/strict';
import {
  DEFAULT_INLINE_VISUALS_ENABLED,
  inlineVisualsEnabled,
  inlineVisualsUpdate,
} from './inlineVisuals.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 触发场景:设置页打开时读 GET /api/config/inline-visuals。
// 期望行为:按响应里的布尔值显示;读不到、响应不是对象、enabled 不是布尔时按 daemon
// 默认值(开启)显示 —— 与配置缺省时系统提示里确实有 visual 说明一致。
run('按 daemon 的响应推出「可视化图表」开关状态', () => {
  assert.equal(DEFAULT_INLINE_VISUALS_ENABLED, true);
  assert.equal(inlineVisualsEnabled({ enabled: true }), true);
  assert.equal(inlineVisualsEnabled({ enabled: false }), false);
  for (const value of [null, undefined, 'false', 0, [], {}, { enabled: 'false' }, { enabled: 0 }]) {
    assert.equal(inlineVisualsEnabled(value), true, JSON.stringify(value));
  }
});

// 触发场景:用户点开关。
// 期望行为:布尔值生成 {enabled} 的 PUT body;其它取值返回 null,组件据此不发请求
// (daemon 对非布尔会回 400)。
run('切换「可视化图表」时的 PUT body', () => {
  assert.deepEqual(inlineVisualsUpdate(true), { enabled: true });
  assert.deepEqual(inlineVisualsUpdate(false), { enabled: false });
  for (const value of [null, undefined, 'true', 1, {}]) {
    assert.equal(inlineVisualsUpdate(value), null, String(value));
  }
});
