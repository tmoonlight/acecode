import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { pointerDragButtonReleased } from './pointerDragGuard.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

// 触发场景:鼠标按住主键拖动,pointermove 的 buttons 含主键(1),或主键加右键(3)。
// 期望行为:不算松开,拖动照常进行。
test('pointer drag guard keeps a real primary-button drag', () => {
  assert.equal(pointerDragButtonReleased({ pointerType: 'mouse', buttons: 1 }), false);
  assert.equal(pointerDragButtonReleased({ pointerType: 'mouse', buttons: 3 }), false);
});

// 触发场景:macOS 开了「轻点以点按」,轻点侧栏会话后松开没被收尾,手指滑动光标,
// pointermove 的 buttons 是 0(没按任何键),或只剩右键(2)。
// 期望行为:判定主键已松开,调用方取消拖动。
// 回归:修复前这种不按键的移动越过 5px 阈值后,会话行被「吸」在光标上跟着走。
test('pointer drag guard treats a mouse move without the primary button as released', () => {
  assert.equal(pointerDragButtonReleased({ pointerType: 'mouse', buttons: 0 }), true);
  assert.equal(pointerDragButtonReleased({ pointerType: 'mouse', buttons: 2 }), true);
});

// 触发场景:触控 / 手写笔的指针事件,或缺 buttons 字段的老旧 / 合成事件,或根本没有事件。
// 期望行为:一律不判定为松开,保持原有拖动行为,不误伤这些输入。
test('pointer drag guard ignores touch, pen and events without buttons', () => {
  assert.equal(pointerDragButtonReleased({ pointerType: 'touch', buttons: 0 }), false);
  assert.equal(pointerDragButtonReleased({ pointerType: 'pen', buttons: 0 }), false);
  assert.equal(pointerDragButtonReleased({ pointerType: 'mouse' }), false);
  assert.equal(pointerDragButtonReleased({ buttons: 0 }), false);
  assert.equal(pointerDragButtonReleased(null), false);
});

// 触发场景:左侧列表的两套自绘拖动(会话行、文件夹行)处理 pointermove。
// 期望行为:先过「主键已松开」守卫,再判断是否越过拖动阈值;守卫命中时会话行走 finishDrag(false)
// 不提交排序,文件夹行收尾后清掉吞 click 标记(松手早已发生,不会再有松手 click)。
// 回归:有人把守卫挪到阈值判断之后或删掉,轻点后滑动光标又会把行拖起来。
test('sidebar session and folder drags check the released button before the drag threshold', () => {
  const sidebar = source('components/Sidebar.jsx');
  const moveStart = sidebar.indexOf('  function onMove(moveEvent) {');
  const moveEnd = sidebar.indexOf('\n  function onUp(upEvent) {', moveStart);
  assert.ok(moveStart >= 0 && moveEnd > moveStart);
  const onMove = sidebar.slice(moveStart, moveEnd);
  const sessionGuard = onMove.indexOf('if (pointerDragButtonReleased(moveEvent)) {\n      finishDrag(false);\n      return;\n    }');
  assert.ok(sessionGuard >= 0);
  assert.ok(sessionGuard < onMove.indexOf('beginDragIfNeeded(moveEvent)'));
  assert.ok(sidebar.includes("import { pointerDragButtonReleased } from '../lib/pointerDragGuard.js';"));

  const folder = source('lib/useWorkspaceFolderReorder.js');
  const folderMoveStart = folder.indexOf('    const move = (moveEvent) => {');
  const folderMoveEnd = folder.indexOf('\n    const up = (upEvent) => {', folderMoveStart);
  assert.ok(folderMoveStart >= 0 && folderMoveEnd > folderMoveStart);
  const folderMove = folder.slice(folderMoveStart, folderMoveEnd);
  const folderGuard = folderMove.indexOf('if (pointerDragButtonReleased(moveEvent)) {\n        cleanup();\n        suppressClickRef.current = false;\n        return;\n      }');
  assert.ok(folderGuard >= 0);
  assert.ok(folderGuard < folderMove.indexOf('Math.hypot('));
  assert.ok(folder.includes("import { pointerDragButtonReleased } from './pointerDragGuard.js';"));
});
