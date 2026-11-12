// 右侧文件树多选后的右键菜单。每一项后面的数字是它实际会处理的条数。
import { joinWorkspacePath, openDesktopContextMenu } from '../lib/desktopContextMenu.js';
import {
  dispatchFileTreeDrop,
  fileTreeDropItems,
  mainComposerFileTreeDropTarget,
} from '../lib/fileTreeDrop.js';
import {
  FILE_TREE_BATCH_ACTIONS,
  fileTreeBatchMenu,
  outermostFileTreeEntries,
} from '../lib/fileTreeSelection.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { toast } from './Toast.jsx';

const LABELS = {
  [FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT]: '添加到会话',
  [FILE_TREE_BATCH_ACTIONS.PREVIEW]: '预览文件',
  [FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS]: '复制相对路径',
  [FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS]: '复制绝对路径',
  [FILE_TREE_BATCH_ACTIONS.EXPAND]: '展开目录',
  [FILE_TREE_BATCH_ACTIONS.COLLAPSE]: '折叠目录',
};

async function copyLines(lines) {
  const result = await copyTextToSystemClipboard(lines.join('\n'));
  if (result?.ok) toast({ kind: 'ok', text: '已复制' });
  else toast({ kind: 'err', text: '复制失败:' + (result?.error || '') });
}

// entries:选中的行 [{ path, name, directory }],按文件树里的先后排列。
export function openFileTreeBatchMenu({
  entries, x, y, element, cwd, expandedDirs, onPreviewFiles, onSetDirectoriesExpanded,
}) {
  const files = entries.filter(entry => !entry.directory);
  const directories = entries.filter(entry => entry.directory);
  const run = {
    [FILE_TREE_BATCH_ACTIONS.ADD_CONTEXT]: () => {
      const added = dispatchFileTreeDrop(
        mainComposerFileTreeDropTarget(element?.ownerDocument),
        fileTreeDropItems(outermostFileTreeEntries(entries), cwd),
        { cwd },
      );
      toast(added ? { kind: 'ok', text: '已添加到会话' } : { kind: 'err', text: '输入框当前不可用' });
    },
    [FILE_TREE_BATCH_ACTIONS.PREVIEW]: () => onPreviewFiles?.(files),
    [FILE_TREE_BATCH_ACTIONS.COPY_RELATIVE_PATHS]: () => copyLines(entries.map(entry => entry.path)),
    [FILE_TREE_BATCH_ACTIONS.COPY_ABSOLUTE_PATHS]: () => copyLines(
      entries.map(entry => joinWorkspacePath(cwd, entry.path)),
    ),
    [FILE_TREE_BATCH_ACTIONS.EXPAND]: () => onSetDirectoriesExpanded?.(
      directories.filter(entry => !expandedDirs.has(entry.path)).map(entry => entry.path), true,
    ),
    [FILE_TREE_BATCH_ACTIONS.COLLAPSE]: () => onSetDirectoriesExpanded?.(
      directories.filter(entry => expandedDirs.has(entry.path)).map(entry => entry.path), false,
    ),
  };
  openDesktopContextMenu({
    x,
    y,
    placement: 'pointer',
    target: element,
    trigger: element,
    includeContextActions: false,
    showIcons: false,
    leadingItems: fileTreeBatchMenu(entries, expandedDirs).map(item => ({
      id: `file-tree-batch-${item.action}`,
      label: `${LABELS[item.action]} (${item.count})`,
      separatorBefore: !!item.separatorBefore,
      onSelect: run[item.action],
    })),
  });
}
