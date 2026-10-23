import assert from 'node:assert/strict';
import { STREAMING_RENDER_INTERVAL_MS, streamingRenderDelay } from './useStreamingRenderThrottle.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:流式消息的第一段文字到达(还没刷新过,lastRenderAt = 0)。
// 期望:不等待,立即显示 —— 限频不能让回答开头延迟出现。
run('first streaming render is immediate', () => {
  assert.equal(streamingRenderDelay(0, 123456), 0);
  assert.equal(streamingRenderDelay(undefined, 123456), 0);
});

// 场景:上次刷新后 30ms 又来了 token。
// 期望:等到上次刷新时刻 + 间隔再刷新(还剩 70ms);token 持续到达时截止时间
// 按上次刷新时刻算,不会被无限推迟。
run('later tokens wait for the remaining interval', () => {
  assert.equal(STREAMING_RENDER_INTERVAL_MS, 100);
  assert.equal(streamingRenderDelay(1000, 1030), 70);
  assert.equal(streamingRenderDelay(1000, 1099), 1);
});

// 场景:距上次刷新已超过一个间隔(模型停顿后又继续输出)。
// 期望:立即刷新,不出现负数延迟。
run('tokens after a full interval render immediately', () => {
  assert.equal(streamingRenderDelay(1000, 1100), 0);
  assert.equal(streamingRenderDelay(1000, 5000), 0);
  assert.equal(streamingRenderDelay(1000, 1010, 20), 10);
});
