// 会话 / 工作区右键菜单的图标与「标记为已读 / 未读」接线合同。

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { INTERFACE_ICONS } from './interfaceIcons.js';
import {
  buildDesktopContextMenuItems,
  DESKTOP_CONTEXT_ACTIONS,
} from './desktopContextMenu.js';

const webRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

function source(relativePath) {
  return fs.readFileSync(path.join(webRoot, 'src', relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

const ACTION_KEY_BY_VALUE = new Map(Object.entries(DESKTOP_CONTEXT_ACTIONS).map(([key, value]) => [value, key]));

// DesktopContextMenu.jsx 里 CONTEXT_ACTION_ICONS 的 { 动作常量名 → 图标名 }。
function contextActionIcons() {
  const block = between(source('components/DesktopContextMenu.jsx'), 'const CONTEXT_ACTION_ICONS', '});');
  return new Map([...block.matchAll(/\[DESKTOP_CONTEXT_ACTIONS\.([A-Z_]+)\]: '([A-Za-z]+)'/g)]
    .map((match) => [match[1], match[2]]));
}

// Icon.jsx 的 ICONS 别名表;不在表里的名字按原样当图标文件名。
function iconFileFor(name) {
  const block = between(source('components/Icon.jsx'), 'const ICONS = {', '};');
  const aliases = new Map([...block.matchAll(/^\s+([A-Za-z]+): '([A-Za-z]+)',/gm)].map((match) => [match[1], match[2]]));
  return aliases.get(name) || name;
}

function iconExists(name) {
  const file = iconFileFor(name);
  return Object.prototype.hasOwnProperty.call(INTERFACE_ICONS, file)
    || fs.existsSync(path.join(webRoot, 'public', 'vs-icons', `${file}.svg`));
}

// 会话 / 工作区目标在各种状态下可能出现的全部菜单项。
function sessionAndWorkspaceActionIds() {
  const ids = new Set();
  for (const pinned of [false, true]) {
    for (const unread of [false, true]) {
      for (const item of buildDesktopContextMenuItems({
        sessionTarget: {
          type: 'session', sessionId: 's1', workspaceHash: 'w1', title: 'T',
          sessionPath: 'C:/p/s1.jsonl', pinned, unread, canArchive: true,
        },
      })) ids.add(item.id);
    }
  }
  for (const expanded of [false, true]) {
    for (const item of buildDesktopContextMenuItems({
      debug: true,
      workspaceTarget: {
        type: 'workspace', workspaceHash: 'w1', name: 'W', path: 'C:/p', active: false, expanded,
        canEdit: true, canRemove: true, opencodeImportCount: 2,
      },
    })) ids.add(item.id);
  }
  return ids;
}

// 场景:侧栏会话行 / 项目行右键。期望:菜单里每一项都有对应的图标(用户要求尽量加
// 图标、尽量复用现有的),且图标名在 Icon.jsx 别名表 / 界面图标 / vs-icons 里真实存在
// —— 写错名字不会报错,只会渲染一个空白方块。
test('会话与工作区右键菜单的每一项都有真实存在的复用图标', () => {
  const icons = contextActionIcons();
  for (const id of sessionAndWorkspaceActionIds()) {
    const key = ACTION_KEY_BY_VALUE.get(id);
    assert.ok(icons.has(key), `missing icon for ${key}`);
    assert.ok(iconExists(icons.get(key)), `icon ${icons.get(key)} for ${key} does not exist`);
  }
  // 已读 / 未读用不同图标,置顶与已读状态不会看起来一样。
  assert.notEqual(icons.get('MARK_SESSION_READ'), icons.get('MARK_SESSION_UNREAD'));
});

// 场景:右键侧栏会话行或项目行。期望:与会话菜单按钮(显式打开)一样显示图标并用宽菜单;
// 其它区域(文件树、消息、输入框)的右键仍是纯文字菜单。回归:原来只有显式打开的菜单
// 才显示图标,右键同一个会话看到的是没有图标的另一种样子。
test('侧栏会话行与项目行的右键菜单显示图标', () => {
  const menu = source('components/DesktopContextMenu.jsx');
  assert.match(menu, /const showIcons = !!explicit \|\| !!\(contextTargets\.sessionTarget \|\| contextTargets\.workspaceTarget\);/);
  assert.match(menu, /const width = showIcons \|\| contextTargets\.previewPresentationTarget \? ICON_MENU_WIDTH : MENU_WIDTH;/);
  assert.match(menu, /showIcons,\n/);
  // 没有图标的项用等宽占位,不再拿 list 图标冒充。
  assert.doesNotMatch(menu, /\|\| 'list'\} size=\{16\}/);
  assert.match(menu, /ace-desktop-context-menu-icon-spacer/);
});

// 场景:会话菜单里点「标记为已读 / 未读」。期望:侧栏行带未读标记供菜单切换文案;侧栏
// 按 session id 处理这两个动作(发 mark_session_read / mark_session_unread 并乐观更新);
// 对当前打开的会话标记未读后,「打开即已读」的自动逻辑在切走之前不会把它改回已读。
test('侧栏接住标记为已读 / 未读并保护当前会话的手动未读', () => {
  const sidebar = source('components/Sidebar.jsx');
  assert.match(sidebar, /data-desktop-session-unread=\{attention === 'unread' \? 'true' : 'false'\}/);
  assert.match(sidebar, /action !== DESKTOP_CONTEXT_ACTIONS\.MARK_SESSION_READ\n\s+&& action !== DESKTOP_CONTEXT_ACTIONS\.MARK_SESSION_UNREAD/);
  assert.match(sidebar, /connection\.markSessionUnread\(/);
  assert.match(sidebar, /optimisticUnreadStatus\(merged\)/);
  const resetAt = sidebar.indexOf("manualUnreadActiveRef.current !== activeId");
  const autoReadAt = sidebar.indexOf("if (manualUnreadActiveRef.current === activeId) return;");
  assert.ok(resetAt > 0 && autoReadAt > resetAt, 'reset effect must run before the auto-read effect');
  assert.match(sidebar, /msg\.type === 'mark_session_unread_ack'/);
  assert.match(source('lib/connection.js'), /type: 'mark_session_unread'/);
  assert.match(source('lib/desktopTaskbarBadge.js'), /message\.type === 'mark_session_unread_ack'/);
});
