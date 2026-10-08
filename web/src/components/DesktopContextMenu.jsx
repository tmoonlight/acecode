import { Fragment, useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { flushSync } from 'react-dom';
import {
  applyContextMenuActionOverrides,
  buildDesktopContextMenuItems,
  canRunContextMenuAction,
  clampContextMenuPosition,
  contextMenuDelegateFromElement,
  contextTargetsFromElement,
  contextMenuOpenDelay,
  dispatchContextMenuDelegate,
  DESKTOP_CONTEXT_ACTION_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  editableTargetFromElement,
  SESSION_PIN_TOGGLE_EVENT,
  OPEN_DESKTOP_CONTEXT_MENU_EVENT,
  CLOSE_DESKTOP_CONTEXT_MENU_EVENT,
  withMenuSeparators,
} from '../lib/desktopContextMenu.js';
import { exportMermaidAsset } from '../lib/mermaidExport.js';
import { selectionContextFromWindowSelection } from '../lib/selectionChatContext.js';
import { copyImageToSystemClipboard, copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { isDesktopShell, isWebappCompat } from '../lib/desktopShellMode.js';
import { notifyNativeSurfaceOverlayChange } from '../lib/agentBrowserSurfaceCoordinator.js';
import {
  captureRichComposerContextSelection,
  insertRichComposerContextText,
  pasteRichComposerContextClipboard,
  richComposerRootFromTarget,
} from '../lib/richComposerContextPaste.js';
import { api } from '../lib/api.js';
import { VsIcon } from './Icon.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';

const MENU_WIDTH = 176;
const ICON_MENU_WIDTH = 216;
const MENU_ROW_HEIGHT = 30;
const MENU_PADDING = 8;

const ACTION_LABELS = {
  [DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER]: '打开文件夹',
  [DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE]: '在资源管理器中显示',
  [DESKTOP_CONTEXT_ACTIONS.PIN_SESSION]: '置顶',
  [DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION]: '取消置顶',
  [DESKTOP_CONTEXT_ACTIONS.MARK_SESSION_READ]: '标记为已读',
  [DESKTOP_CONTEXT_ACTIONS.MARK_SESSION_UNREAD]: '标记为未读',
  [DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION]: '打开会话',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION]: '重命名',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE]: '复制标题',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID]: '复制ID',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION]: '导出',
  [DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION]: '归档',
  [DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE]: '切换到项目',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE]: '展开项目',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_WORKSPACE]: '折叠项目',
  [DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION]: '新建会话',
  [DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS]: '从opencode导入会话',
  [DESKTOP_CONTEXT_ACTIONS.EDIT_WORKSPACE]: '编辑项目',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE]: '重命名项目',
  [DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH]: '复制项目路径',
  [DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE]: '从项目列表移除',
  [DESKTOP_CONTEXT_ACTIONS.PREVIEW_FILE]: '预览文件',
  [DESKTOP_CONTEXT_ACTIONS.REFRESH_DETAILS]: '刷新',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TAB]: '关闭',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_OTHER_PREVIEW_TABS]: '关闭其他',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_PREVIEW_TABS_TO_RIGHT]: '关闭右侧标签页',
  [DESKTOP_CONTEXT_ACTIONS.CLOSE_ALL_PREVIEW_TABS]: '全部关闭',
  [DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH]: '复制相对路径',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH]: '复制绝对路径',
  [DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT]: '添加到会话',
  [DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT]: '添加到会话',
  [DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT]: '引用到聊天',
  [DESKTOP_CONTEXT_ACTIONS.TOGGLE_PREVIEW_PRESENTATION]: '全屏演示',
  [DESKTOP_CONTEXT_ACTIONS.REFRESH_FILE_TREE]: '刷新文件树',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_DIRECTORY]: '展开目录',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_DIRECTORY]: '折叠目录',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_TEXT]: '复制预览内容',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_METADATA]: '复制预览信息',
  [DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_IMAGE]: '复制图片',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_PNG]: '导出 PNG 图片',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SVG]: '导出 SVG',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SOURCE]: '导出 Mermaid 源码',
  [DESKTOP_CONTEXT_ACTIONS.COPY_FILE_DIFF]: '复制此文件 diff',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ALL_DIFFS]: '复制全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.LOCATE_IN_FILE_TREE]: '在文件树中定位',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_ALL_DIFFS]: '展开全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_ALL_DIFFS]: '折叠全部 diff',
  [DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT]: '复制消息',
  [DESKTOP_CONTEXT_ACTIONS.FORK_MESSAGE]: '从这里分叉',
  [DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT]: '复制可见输出',
  [DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT]: '复制完整输出',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_TOOL]: '展开工具详情',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_TOOL]: '收起工具详情',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE]: '复制图片',
  [DESKTOP_CONTEXT_ACTIONS.PREVIEW_ATTACHMENT]: '预览附件',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME]: '复制附件名',
  [DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL]: '复制附件地址',
  [DESKTOP_CONTEXT_ACTIONS.REMOVE_ATTACHMENT]: '移除附件',
  [DESKTOP_CONTEXT_ACTIONS.SELECT_ALL]: '全选',
  [DESKTOP_CONTEXT_ACTIONS.COPY]: '复制',
  [DESKTOP_CONTEXT_ACTIONS.PASTE]: '粘贴',
  [DESKTOP_CONTEXT_ACTIONS.CUT]: '剪切',
  [DESKTOP_CONTEXT_ACTIONS.INSPECT]: '检查',
};

// 会话 / 工作区菜单的图标,全部复用 Icon.jsx 里已有的界面图标。会话菜单按钮 / 顶栏右键
// (显式打开)与侧栏会话行、项目行的右键共用这一份;其它右键菜单(文件、消息等)不显示图标。
const CONTEXT_ACTION_ICONS = Object.freeze({
  [DESKTOP_CONTEXT_ACTIONS.PIN_SESSION]: 'pin',
  [DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION]: 'pin',
  [DESKTOP_CONTEXT_ACTIONS.MARK_SESSION_READ]: 'check',
  [DESKTOP_CONTEXT_ACTIONS.MARK_SESSION_UNREAD]: 'StatusNotStarted',
  [DESKTOP_CONTEXT_ACTIONS.OPEN_SESSION]: 'chat',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_SESSION]: 'edit',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE]: 'copy',
  [DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID]: 'copy',
  [DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION]: 'Download',
  [DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER]: 'folderOpen',
  [DESKTOP_CONTEXT_ACTIONS.ARCHIVE_SESSION]: 'archive',
  [DESKTOP_CONTEXT_ACTIONS.ACTIVATE_WORKSPACE]: 'arrowRight',
  [DESKTOP_CONTEXT_ACTIONS.EXPAND_WORKSPACE]: 'expandDown',
  [DESKTOP_CONTEXT_ACTIONS.COLLAPSE_WORKSPACE]: 'expandRight',
  [DESKTOP_CONTEXT_ACTIONS.NEW_WORKSPACE_SESSION]: 'newSession',
  [DESKTOP_CONTEXT_ACTIONS.IMPORT_OPENCODE_SESSIONS]: 'Download',
  [DESKTOP_CONTEXT_ACTIONS.EDIT_WORKSPACE]: 'editWindow',
  [DESKTOP_CONTEXT_ACTIONS.RENAME_WORKSPACE]: 'edit',
  [DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH]: 'copy',
  [DESKTOP_CONTEXT_ACTIONS.REMOVE_WORKSPACE]: 'close',
  [DESKTOP_CONTEXT_ACTIONS.SELECT_ALL]: 'list',
  [DESKTOP_CONTEXT_ACTIONS.COPY]: 'copy',
  [DESKTOP_CONTEXT_ACTIONS.INSPECT]: 'Inspect',
});

function parseDesktopResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

function selectedTextForTarget(target) {
  const editable = editableTargetFromElement(target);
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    const start = editable.selectionStart ?? 0;
    const end = editable.selectionEnd ?? start;
    return start === end ? '' : editable.value.slice(start, end);
  }
  return window.getSelection?.().toString() || '';
}

function selectEditableContents(editable) {
  editable.focus();
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    editable.select();
    return;
  }
  const range = document.createRange();
  range.selectNodeContents(editable);
  const sel = window.getSelection();
  sel?.removeAllRanges();
  sel?.addRange(range);
}

function selectAllForTarget(target) {
  const editable = editableTargetFromElement(target);
  if (editable) {
    selectEditableContents(editable);
    return;
  }
  const sel = window.getSelection();
  if (!sel || !document.body) return;
  sel.removeAllRanges();
  const range = document.createRange();
  range.selectNodeContents(document.body);
  sel.addRange(range);
}

function insertTextIntoEditable(editable, text, richComposerSelection = null) {
  if (editable instanceof HTMLInputElement || editable instanceof HTMLTextAreaElement) {
    editable.focus();
    const start = editable.selectionStart ?? editable.value.length;
    const end = editable.selectionEnd ?? start;
    editable.setRangeText(text, start, end, 'end');
    editable.dispatchEvent(new InputEvent('input', {
      bubbles: true,
      cancelable: true,
      data: text,
      inputType: 'insertFromPaste',
    }));
    return;
  }
  if (insertRichComposerContextText(editable, text, richComposerSelection)) return;
  if (richComposerRootFromTarget(editable)) return;
  editable.focus();
  document.execCommand('insertText', false, text);
}

async function copySelectionFromTarget(target, rememberedText = '') {
  const text = selectedTextForTarget(target) || rememberedText;
  if (text) {
    const result = await copyTextToSystemClipboard(text);
    if (!result?.ok) throw new Error(result?.error || 'clipboard unavailable');
    return;
  }
  document.execCommand('copy');
}

async function copyTextWithToast(text, label = '已复制') {
  const result = await copyTextToSystemClipboard(text);
  if (result?.ok) {
    toast({ kind: 'ok', text: label });
  } else {
    toast({ kind: 'err', text: '复制失败:' + (result?.error || '') });
  }
}

async function exportMermaidWithToast(target, format, label) {
  const result = await exportMermaidAsset(target, format);
  if (result?.ok) {
    toast({ kind: 'ok', text: `已导出 ${label}` });
  } else {
    toast({ kind: 'err', text: '导出失败:' + (result?.error || '') });
  }
  return result;
}

export async function copyImageWithToast(target) {
  const result = await copyImageToSystemClipboard(target?.copyImageUrl || target?.previewUrl || '', {
    mimeType: target?.mimeType || target?.contentType || '',
  });
  if (result?.ok) {
    toast({ kind: 'ok', text: '已复制图片' });
  } else {
    toast({ kind: 'err', text: '复制图片失败:' + (result?.error || '') });
  }
  return result;
}

async function pasteIntoTarget(target, richComposerSelection = null) {
  const editable = editableTargetFromElement(target);
  if (!editable) return;
  const paste = pasteRichComposerContextClipboard(editable, richComposerSelection);
  if (paste) {
    await paste;
    return;
  }
  if (navigator.clipboard?.readText) {
    const text = await navigator.clipboard.readText();
    insertTextIntoEditable(editable, text, richComposerSelection);
    return;
  }
  if (richComposerRootFromTarget(editable)) return;
  editable.focus();
  document.execCommand('paste');
}

async function openTargetInExplorer(openTarget) {
  if (!openTarget?.path) {
    toast({ kind: 'err', text: '无法打开:路径为空' });
    return;
  }
  try {
    let result;
    if (typeof window.aceDesktop_openInExplorer === 'function') {
      result = parseDesktopResult(await window.aceDesktop_openInExplorer(openTarget.path));
    } else {
      // webapp 兼容模式没有 webview bridge,daemon 就在本机,改走 REST 端点。
      result = await api.openInExplorer(openTarget.path);
    }
    if (!result?.ok) {
      toast({ kind: 'err', text: '打开失败:' + (result?.error || '') });
      return;
    }
    toast({ kind: 'ok', text: '已打开文件夹' });
  } catch (e) {
    const detail = e?.body?.error || e?.message || '';
    toast({ kind: 'err', text: '打开失败:' + detail });
  }
}

function dispatchSessionPinToggle(sessionPinTarget, nextPinned) {
  if (!sessionPinTarget?.sessionId) return;
  window.dispatchEvent(new CustomEvent(SESSION_PIN_TOGGLE_EVENT, {
    detail: {
      ...sessionPinTarget,
      pinned: !!nextPinned,
    },
  }));
}

function dispatchDesktopContextAction(action, targetPayload, extra = {}) {
  const detail = {
    action,
    target: targetPayload || null,
    handled: false,
    ...extra,
  };
  window.dispatchEvent(new CustomEvent(DESKTOP_CONTEXT_ACTION_EVENT, { detail }));
  return !!detail.handled;
}

async function runAction(
  item,
  target,
  rememberedText = '',
  rememberedSelectionContext = null,
  rememberedRichComposerSelection = null,
  { allowNativeActions = false } = {},
) {
  const action = typeof item === 'string' ? item : item?.id;
  const actionTarget = typeof item === 'object' ? item.target : null;
  if (!action) return;
  if (!canRunContextMenuAction(action, { allowNativeActions })) return;

  if (typeof item?.onSelect === 'function') {
    await item.onSelect();
    return;
  }

  switch (action) {
    case DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER:
      await openTargetInExplorer(actionTarget);
      break;
    case DESKTOP_CONTEXT_ACTIONS.LOCATE_FILE:
      await openTargetInExplorer({ path: actionTarget?.locatePath, kind: 'directory' });
      break;
    case DESKTOP_CONTEXT_ACTIONS.PIN_SESSION:
      dispatchSessionPinToggle(actionTarget, true);
      break;
    case DESKTOP_CONTEXT_ACTIONS.UNPIN_SESSION:
      dispatchSessionPinToggle(actionTarget, false);
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_TITLE:
      await copyTextWithToast(actionTarget?.title || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_SESSION_ID:
      await copyTextWithToast(actionTarget?.sessionId || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_SESSION:
      try {
        const result = await api.exportSession(
          actionTarget?.sessionId || '',
          actionTarget?.workspaceHash || '',
        );
        if (result?.cancelled) break;
        if (result?.ok) {
          toast({ kind: 'ok', text: `会话已导出:${result.filename || 'session.md'}` });
        } else {
          toast({ kind: 'err', text: '导出会话失败:' + (result?.error || '') });
        }
      } catch (e) {
        const detail = e?.body?.error || e?.message || '';
        toast({ kind: 'err', text: '导出会话失败:' + detail });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_WORKSPACE_PATH:
      await copyTextWithToast(actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_RELATIVE_PATH:
      await copyTextWithToast(actionTarget?.relativePath || actionTarget?.file || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH:
      await copyTextWithToast(actionTarget?.absolutePath || actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_MESSAGE_TEXT:
      await copyTextWithToast(actionTarget?.text || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_VISIBLE_TOOL_OUTPUT:
      await copyTextWithToast(actionTarget?.visibleOutput || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_FULL_TOOL_OUTPUT:
      await copyTextWithToast(actionTarget?.fullOutput || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_NAME:
      await copyTextWithToast(actionTarget?.name || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_URL:
      await copyTextWithToast(actionTarget?.url || actionTarget?.path || '');
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY_ATTACHMENT_IMAGE:
    case DESKTOP_CONTEXT_ACTIONS.COPY_PREVIEW_IMAGE:
      await copyImageWithToast(actionTarget);
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_PNG:
      await exportMermaidWithToast(actionTarget, 'png', 'PNG 图片');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SVG:
      await exportMermaidWithToast(actionTarget, 'svg', 'SVG');
      break;
    case DESKTOP_CONTEXT_ACTIONS.EXPORT_MERMAID_SOURCE:
      await exportMermaidWithToast(actionTarget, 'source', 'Mermaid 源码');
      break;
    case DESKTOP_CONTEXT_ACTIONS.ADD_SELECTION_CONTEXT:
      if (!dispatchDesktopContextAction(action, actionTarget, {
        selectedText: rememberedText,
        selectionContext: rememberedSelectionContext,
      })) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.ADD_FILE_CONTEXT:
    case DESKTOP_CONTEXT_ACTIONS.ADD_DIRECTORY_CONTEXT:
      if (!dispatchDesktopContextAction(action, actionTarget)) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.SELECT_ALL:
      selectAllForTarget(target);
      break;
    case DESKTOP_CONTEXT_ACTIONS.COPY:
      try {
        await copySelectionFromTarget(target, rememberedText);
        toast({ kind: 'ok', text: '已复制' });
      } catch (e) {
        toast({ kind: 'err', text: '复制失败:' + (e?.message || '') });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.PASTE:
      try {
        await pasteIntoTarget(target, rememberedRichComposerSelection);
      } catch (error) {
        toast({ kind: 'err', text: `操作失败：${error?.message || ''}` });
      }
      break;
    case DESKTOP_CONTEXT_ACTIONS.CUT:
      editableTargetFromElement(target)?.focus();
      document.execCommand('cut');
      break;
    case DESKTOP_CONTEXT_ACTIONS.INSPECT:
      await window.aceDesktop_openDevTools?.();
      break;
    default:
      if (!dispatchDesktopContextAction(action, actionTarget)) {
        toast({ kind: 'err', text: '操作不可用' });
      }
      break;
  }
}

function actionLabel(action) {
  const id = typeof action === 'string' ? action : action.id;
  if (id === DESKTOP_CONTEXT_ACTIONS.TOGGLE_PREVIEW_PRESENTATION && action.target?.active) return '退出全屏演示';
  return action?.label || ACTION_LABELS[id] || id;
}

export function DesktopContextMenu() {
  const [menu, setMenuState] = useState(null);
  const [pendingConfirm, setPendingConfirm] = useState(null);
  const menuRef = useRef(null);
  const menuElementRef = useRef(null);
  const reopenTimerRef = useRef(0);
  const targetRef = useRef(null);
  const lastSelectionRef = useRef({ target: null, text: '' });
  // 所有运行模式共享自定义菜单；只有 Desktop Shell / Edge WebApp 兼容模式
  // 可以展示和执行资源管理器、DevTools 等 native-only 动作。
  const allowNativeActions = useMemo(() => isDesktopShell() || isWebappCompat(), []);

  const setMenu = useCallback((nextMenu) => {
    menuRef.current = nextMenu;
    setMenuState(nextMenu);
  }, []);

  const clearReopenTimer = useCallback(() => {
    if (!reopenTimerRef.current) return;
    window.clearTimeout(reopenTimerRef.current);
    reopenTimerRef.current = 0;
  }, []);

  const close = useCallback(() => {
    clearReopenTimer();
    setMenu(null);
  }, [clearReopenTimer, setMenu]);

  const openWithSwitchGap = useCallback((nextMenu) => {
    const delay = contextMenuOpenDelay({
      hasVisibleMenu: !!menuRef.current,
      hasPendingMenu: !!reopenTimerRef.current,
    });
    clearReopenTimer();
    if (delay <= 0) {
      setMenu(nextMenu);
      return;
    }
    setMenu(null);
    reopenTimerRef.current = window.setTimeout(() => {
      reopenTimerRef.current = 0;
      setMenu(nextMenu);
    }, delay);
  }, [clearReopenTimer, setMenu]);

  useEffect(() => {
    const rememberSelection = () => {
      const target = document.activeElement;
      const text = selectedTextForTarget(target);
      if (text) lastSelectionRef.current = { target: editableTargetFromElement(target) || target, text };
    };
    const closeFromPointer = (event) => {
      if (event.target instanceof Element && event.target.closest('.ace-desktop-context-menu')) return;
      close();
    };
    const closeOnOutsidePress = (event) => {
      if (!menuRef.current && !reopenTimerRef.current) return;
      if (menuElementRef.current?.contains(event.target)) return;
      // Native title-bar dragging can consume mouseup/click and block repaint.
      // Dismiss before mousedown enters the native bridge; leave the press intact.
      flushSync(close);
    };
    const onContextMenu = (event) => {
      // 控制台终端区由 ConsoleDock 自己处理右键(VS Code 式 复制/粘贴),这里放行
      // (不 preventDefault / 不 stopPropagation),让事件继续冒泡到终端的 onContextMenu。
      const explicit = event.type === OPEN_DESKTOP_CONTEXT_MENU_EVENT ? event.detail : null;
      const rawTarget = explicit?.target || event.target;
      if (rawTarget instanceof Element && rawTarget.closest('.ace-console-term')) return;

      // 顶栏等区域右键委托给认领方(会话头部 = 与「会话菜单」按钮左键同一份菜单);
      // 没人认领(例如首页没有打开会话)时回落到通用菜单。
      if (!explicit) {
        const delegate = contextMenuDelegateFromElement(rawTarget);
        if (delegate && dispatchContextMenuDelegate(delegate, {
          x: event.clientX,
          y: event.clientY,
          target: rawTarget,
        })) {
          event.preventDefault();
          event.stopPropagation();
          return;
        }
      }

      const candidateTargets = contextTargetsFromElement(rawTarget);
      event.preventDefault();
      event.stopPropagation();

      const target = rawTarget;
      targetRef.current = target;
      const editableTarget = editableTargetFromElement(target);
      const editable = !!editableTarget;
      const richComposerSelection = editableTarget
        && !(editableTarget instanceof HTMLInputElement)
        && !(editableTarget instanceof HTMLTextAreaElement)
        ? captureRichComposerContextSelection(editableTarget)
        : null;
      const contextTargets = editable
        ? { previewTarget: candidateTargets.previewTarget || null, previewPresentationTarget: candidateTargets.previewPresentationTarget }
        : candidateTargets;
      const sessionPinTarget = contextTargets.sessionTarget
        ? {
            sessionId: contextTargets.sessionTarget.sessionId,
            workspaceHash: contextTargets.sessionTarget.workspaceHash,
            pinned: contextTargets.sessionTarget.pinned,
          }
        : null;
      let selectedText = selectedTextForTarget(target);
      if (!selectedText && editableTarget && lastSelectionRef.current.target === editableTarget) {
        selectedText = lastSelectionRef.current.text;
      }
      const hasSelection = selectedText.length > 0;
      const selectionContext = hasSelection
        ? selectionContextFromWindowSelection({ target, selectedText })
        : null;
      const debug = !!window.__ACECODE_DESKTOP_DEBUG__;
      const contextItems = explicit?.includeContextActions === false ? [] : buildDesktopContextMenuItems({
        editable,
        hasSelection,
        debug,
        allowNativeActions,
        ...contextTargets,
        sessionPinTarget,
      });
      const items = withMenuSeparators(applyContextMenuActionOverrides(
        [...(explicit?.leadingItems || []), ...contextItems],
        explicit?.actionOverrides,
      ));
      // 侧栏会话行 / 项目行的右键与会话菜单按钮一样带图标;其它区域的右键保持纯文字。
      const showIcons = !!explicit || !!(contextTargets.sessionTarget || contextTargets.workspaceTarget);
      const width = showIcons || contextTargets.previewPresentationTarget ? ICON_MENU_WIDTH : MENU_WIDTH;
      // 按钮触发时菜单右缘对齐按钮;右键委托(placement=pointer)时像原生右键一样从光标处展开。
      const x = explicit
        ? (explicit.placement === 'pointer' ? explicit.x : explicit.x - width)
        : event.clientX;
      const y = explicit ? explicit.y : event.clientY;
      const pos = clampContextMenuPosition({
        x, y, width,
        height: items.length * MENU_ROW_HEIGHT + MENU_PADDING + items.filter((item) => item.separatorBefore).length * 9,
        viewportWidth: window.innerWidth,
        viewportHeight: window.innerHeight,
      });
      openWithSwitchGap({
        ...pos,
        x, y, width,
        trigger: explicit?.trigger,
        showIcons,
        items,
        selectedText,
        selectionContext,
        richComposerSelection,
      });
    };
    const onKeyDown = (event) => {
      const currentMenu = menuRef.current;
      if (!currentMenu) return;
      if (event.key === 'Escape') {
        event.preventDefault();
        event.stopPropagation();
        close();
        currentMenu.trigger?.focus();
      } else if (event.key === 'Tab') {
        close();
        currentMenu.trigger?.focus();
      } else if (['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) {
        const buttons = [...(menuElementRef.current?.querySelectorAll('[role="menuitem"]:not(:disabled)') || [])];
        if (!buttons.length) return;
        event.preventDefault();
        event.stopPropagation();
        const index = buttons.indexOf(document.activeElement);
        const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1
          : index < 0 ? (event.key === 'ArrowDown' ? 0 : buttons.length - 1)
          : (index + (event.key === 'ArrowDown' ? 1 : -1) + buttons.length) % buttons.length;
        buttons[next].focus();
      }
    };

    document.addEventListener(OPEN_DESKTOP_CONTEXT_MENU_EVENT, onContextMenu);
    document.addEventListener(CLOSE_DESKTOP_CONTEXT_MENU_EVENT, close);
    document.addEventListener('contextmenu', onContextMenu, true);
    document.addEventListener('selectionchange', rememberSelection);
    document.addEventListener('select', rememberSelection, true);
    document.addEventListener('click', closeFromPointer, true);
    document.addEventListener('wheel', closeFromPointer, true);
    document.addEventListener('keydown', onKeyDown, true);
    window.addEventListener('pointerdown', closeOnOutsidePress, true);
    window.addEventListener('blur', close);
    window.addEventListener('resize', close);
    return () => {
      document.removeEventListener(OPEN_DESKTOP_CONTEXT_MENU_EVENT, onContextMenu);
      document.removeEventListener(CLOSE_DESKTOP_CONTEXT_MENU_EVENT, close);
      document.removeEventListener('contextmenu', onContextMenu, true);
      document.removeEventListener('selectionchange', rememberSelection);
      document.removeEventListener('select', rememberSelection, true);
      document.removeEventListener('click', closeFromPointer, true);
      document.removeEventListener('wheel', closeFromPointer, true);
      document.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('pointerdown', closeOnOutsidePress, true);
      window.removeEventListener('blur', close);
      window.removeEventListener('resize', close);
      clearReopenTimer();
    };
  }, [allowNativeActions, clearReopenTimer, close, openWithSwitchGap]);

  useLayoutEffect(() => {
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [menu]);

  useLayoutEffect(() => {
    const element = menuElementRef.current;
    if (!menu || !element) return;
    const rect = element.getBoundingClientRect();
    const position = clampContextMenuPosition({
      x: menu.x, y: menu.y, width: rect.width, height: rect.height,
      viewportWidth: window.innerWidth, viewportHeight: window.innerHeight,
    });
    if (position.left !== menu.left || position.top !== menu.top) setMenu({ ...menu, ...position });
    if (menu.trigger && !element.contains(document.activeElement)) {
      element.querySelector('[role="menuitem"]:not(:disabled)')?.focus();
    }
  }, [menu, setMenu]);

  if (!menu && !pendingConfirm) return null;

  return (
    <>
      {menu && (
        <div
          ref={menuElementRef}
          className="ace-desktop-context-menu"
          data-ace-native-overlay="overlap"
          style={{ left: menu.left, top: menu.top, width: menu.width }}
          role="menu"
          onMouseDown={(event) => event.stopPropagation()}
          onClick={(event) => event.stopPropagation()}
        >
          {menu.items.map((action) => (
            <Fragment key={typeof action === 'string' ? action : action.id}>
              {action.separatorBefore && <div role="separator" className="ace-desktop-context-menu-separator" />}
              <button
                type="button"
                role="menuitem"
                disabled={typeof action === 'object' && action.enabled === false}
                className={[
                  'ace-desktop-context-menu-item',
                  typeof action === 'object' && action.danger ? 'ace-desktop-context-menu-danger' : '',
                ].filter(Boolean).join(' ')}
                onClick={async () => {
                  if (typeof action === 'object' && action.enabled === false) return;
                  const target = targetRef.current;
                  const selectedText = menu.selectedText || '';
                  const selectionContext = menu.selectionContext || null;
                  const richComposerSelection = menu.richComposerSelection || null;
                  close();
                  menu.trigger?.focus();
                  if (typeof action === 'object' && action.confirm) {
                    setPendingConfirm({
                      action,
                      target,
                      selectedText,
                      selectionContext,
                      richComposerSelection,
                    });
                    return;
                  }
                  await runAction(
                    action,
                    target,
                    selectedText,
                    selectionContext,
                    richComposerSelection,
                    { allowNativeActions },
                  );
                }}
              >
                {menu.showIcons && (action.icon || CONTEXT_ACTION_ICONS[action.id]
                  ? <VsIcon name={action.icon || CONTEXT_ACTION_ICONS[action.id]} size={16} />
                  : <span className="ace-desktop-context-menu-icon-spacer" aria-hidden="true" />)}
                <span>{actionLabel(action)}</span>
                {action.id === DESKTOP_CONTEXT_ACTIONS.TOGGLE_PREVIEW_PRESENTATION && (
                  <span className="ml-auto pl-4 text-fg-mute" aria-hidden="true">Ctrl+F11</span>
                )}
              </button>
            </Fragment>
          ))}
        </div>
      )}
      {pendingConfirm && (
        <Modal onClose={() => setPendingConfirm(null)} width={440} layerClassName="z-[400]">
          {({ close: closeConfirm }) => (
            <div className="p-4">
              <div className="text-[14px] font-semibold mb-2">{actionLabel(pendingConfirm.action)}</div>
              <div className="text-[12.5px] text-fg-mute leading-relaxed mb-4">
                {pendingConfirm.action.confirm}
              </div>
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-border hover:bg-surface-hi"
                  onClick={closeConfirm}
                >
                  取消
                </button>
                <button
                  type="button"
                  data-ace-dialog-primary="true"
                  className="px-3 py-1.5 text-[12.5px] rounded-lg border border-danger/40 bg-danger-bg text-danger hover:opacity-80"
                  onClick={async () => {
                    const pending = pendingConfirm;
                    setPendingConfirm(null);
                    await runAction(
                      pending.action,
                      pending.target,
                      pending.selectedText,
                      pending.selectionContext,
                      pending.richComposerSelection,
                      { allowNativeActions },
                    );
                  }}
                >
                  确认
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
    </>
  );
}
