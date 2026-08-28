import assert from 'node:assert/strict';
import {
  opencodePreviewTargets,
  pinnedRefreshTargets,
} from './sidebarAuxiliaryFetch.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const WORKSPACES = [
  { hash: 'active' },
  { hash: 'expanded' },
  { hash: 'collapsed-a' },
  { hash: 'collapsed-b' },
  { hash: '__local__' },
];
const VISIBLE = ['active', 'expanded', '__local__'];

// 触发场景:侧边栏每 5 秒 refresh 一次,而用户有十几个 workspace。
// 期望行为:只对可见的那几个重取 pinned —— 这正是这次改动要压掉的稳态请求量。
// 回归背景:原实现对全部 workspace 各发一次,14 个 workspace 就把浏览器仅有的
// 6 条并发连接占满,用户点击展开时那个会话列表请求排队 400ms+,侧边栏长时间
// 停在「加载中...」。
test('pinnedRefreshTargets 只取可见 workspace', () => {
  assert.deepEqual(pinnedRefreshTargets(WORKSPACES, VISIBLE), ['active', 'expanded', '__local__']);
});

// 触发场景:折叠的 workspace 不在本轮请求里。
// 期望行为:函数不返回它们,调用方据此沿用缓存值。这里守住的是「不返回」,
// 「不清空」由 Sidebar 侧的分支保证 —— 两者一起才不会让展开时置顶标记闪一下。
test('pinnedRefreshTargets 不返回折叠的 workspace', () => {
  const targets = pinnedRefreshTargets(WORKSPACES, VISIBLE);
  assert.equal(targets.includes('collapsed-a'), false);
  assert.equal(targets.includes('collapsed-b'), false);
});

// 触发场景:首轮 refresh,一个 workspace 都还没探过 opencode 导入预览。
// 期望行为:全部探一遍(除 __local__,它没有对应项目目录)—— 导入提示不能因为
// 这次优化而在折叠的 workspace 上丢失。
test('opencodePreviewTargets 首轮覆盖全部 workspace', () => {
  assert.deepEqual(
    opencodePreviewTargets(WORKSPACES, VISIBLE, []),
    ['active', 'expanded', 'collapsed-a', 'collapsed-b'],
  );
});

// 触发场景:稳态 —— 全部探过之后的每一轮。
// 期望行为:只重探可见的。opencode 导入预览探的是「这个目录有没有可导入数据」,
// 近乎静态,5 秒一次对全部 workspace 重探纯属浪费。
test('opencodePreviewTargets 稳态只重探可见的', () => {
  const probed = ['active', 'expanded', 'collapsed-a', 'collapsed-b'];
  assert.deepEqual(opencodePreviewTargets(WORKSPACES, VISIBLE, probed), ['active', 'expanded']);
});

// 触发场景:新增一个 workspace 后的那一轮,它既不可见也没被探过。
// 期望行为:仍要探一次,否则新 workspace 永远拿不到导入提示。
test('opencodePreviewTargets 会探新出现的折叠 workspace', () => {
  const probed = ['active', 'expanded'];
  assert.deepEqual(
    opencodePreviewTargets(WORKSPACES, VISIBLE, probed),
    ['active', 'expanded', 'collapsed-a', 'collapsed-b'],
  );
});

// 触发场景:空输入 / 缺 hash 的脏数据。
// 期望行为:不抛异常,不产出空 hash。
test('两个函数都容忍空输入与缺 hash 的条目', () => {
  assert.deepEqual(pinnedRefreshTargets(), []);
  assert.deepEqual(opencodePreviewTargets(), []);
  assert.deepEqual(pinnedRefreshTargets([{ hash: '' }, {}, null], ['x']), []);
  assert.deepEqual(opencodePreviewTargets([{ hash: '  ' }, {}], ['x'], []), []);
});
