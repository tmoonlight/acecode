import assert from 'node:assert/strict';
import { submenuPosition } from './submenuPosition.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const viewport = { viewportWidth: 1200, viewportHeight: 800 };

// 场景:Agent Browser 的「⋮」菜单在工具栏右侧打开,「保留 Cookie」子菜单展开。
// 期望:向左展开(与 Claude 桌面版一致),右边缘离父菜单 4px,顶边与触发项对齐。
await run('submenu opens to the left of a right-edge menu', () => {
  const position = submenuPosition({
    menuRect: { left: 900, right: 1180, top: 60, bottom: 260 },
    itemRect: { top: 120, bottom: 152 },
    submenuWidth: 300,
    submenuHeight: 180,
    ...viewport,
  });
  assert.deepEqual(position, { left: 596, top: 116, side: 'left' });
});

// 场景:窗口很窄,父菜单左侧放不下子菜单。
// 期望:改到右侧;两侧都放不下时贴视口边缘,不能跑出窗口。
await run('submenu flips or clamps when the preferred side has no room', () => {
  const flipped = submenuPosition({
    menuRect: { left: 40, right: 320, top: 60, bottom: 260 },
    itemRect: { top: 100, bottom: 132 },
    submenuWidth: 300,
    submenuHeight: 100,
    ...viewport,
  });
  assert.equal(flipped.side, 'right');
  assert.equal(flipped.left, 324);

  const clamped = submenuPosition({
    menuRect: { left: 100, right: 380, top: 60, bottom: 260 },
    itemRect: { top: 100, bottom: 132 },
    submenuWidth: 300,
    submenuHeight: 100,
    viewportWidth: 500,
    viewportHeight: 800,
  });
  assert.ok(clamped.left >= 8 && clamped.left + 300 <= 492, JSON.stringify(clamped));
});

// 场景:触发项靠近窗口底部。
// 期望:子菜单整体上移,底边不超出视口;顶边也不低于 8px 边距。
await run('submenu stays inside the viewport vertically', () => {
  const position = submenuPosition({
    menuRect: { left: 900, right: 1180, top: 500, bottom: 790 },
    itemRect: { top: 760, bottom: 790 },
    submenuWidth: 300,
    submenuHeight: 180,
    ...viewport,
  });
  assert.equal(position.top, 800 - 8 - 180);
  const tall = submenuPosition({
    menuRect: { left: 900, right: 1180, top: 0, bottom: 300 },
    itemRect: { top: 2, bottom: 30 },
    submenuWidth: 300,
    submenuHeight: 900,
    ...viewport,
  });
  assert.equal(tall.top, 8);
});
