import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import {
  CHANGE_LIST_VIEW_TREE,
  buildChangeFileTree,
  changeTreeAncestorPaths,
  flattenVisibleChangeTree,
  normalizeChangeTreePath,
} from '../lib/changeFileTree.js';
import { joinWorkspacePath } from '../lib/desktopContextMenu.js';
import { normalizeTreePath } from '../lib/fileTreeChangeStatus.js';
import { clsx } from '../lib/format.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';

function splitPath(path) {
  const parts = String(path || '').split(/[\\/]/).filter(Boolean);
  const name = parts.pop() || String(path || '');
  return { name, parent: parts.join('/') };
}

function statusBadgeClass(status) {
  switch (status) {
    case 'A': return 'text-ok';
    case 'D': return 'text-danger';
    case 'R': return 'text-warn';
    default:  return 'text-fg-mute';
  }
}

function ChangeCounts({ row }) {
  const statLabel = typeof row?.statLabel === 'string' ? row.statLabel.trim() : '';
  const statParts = statLabel.match(/^(\+\S+)\s+(-\S+)$/);
  if (statParts) {
    return (
      <span className="ace-change-file-counts">
        <span className="ace-change-add">{statParts[1]}</span>
        <span className="ace-change-del">{statParts[2]}</span>
      </span>
    );
  }
  if (statLabel) {
    return (
      <span className="ace-change-file-counts">
        <span className="text-fg-mute">{statLabel}</span>
      </span>
    );
  }

  const additions = Number(row?.additions || 0);
  const deletions = Number(row?.deletions || 0);
  return (
    <span className="ace-change-file-counts">
      {additions > 0 && <span className="ace-change-add">+{additions}</span>}
      {deletions > 0 && <span className="ace-change-del">-{deletions}</span>}
    </span>
  );
}

function FileMarker({ row, showStatus, tree }) {
  if (showStatus) {
    return (
      <span className={clsx('ace-change-status-badge', statusBadgeClass(row.status))}>
        {row.status || 'M'}
      </span>
    );
  }
  return tree
    ? <FileTypeIcon path={row.path} size={17} />
    : <VsIcon name="file" size={14} mono={false} />;
}

function OpenPreviewAction({ row, onOpenFilePreview, guardDeletedPreview }) {
  if (!onOpenFilePreview) return null;
  const unavailable = guardDeletedPreview && row.status === 'D';
  return (
    // Deleted files do not exist on disk. Keep an invisible placeholder so
    // the status/count columns stay aligned with revealable rows.
    <span
      role="button"
      tabIndex={-1}
      className={clsx('ace-change-row-action', unavailable && 'is-hidden')}
      title="转到文件"
      aria-label={`转到文件 ${row.path}`}
      onPointerDown={(event) => event.stopPropagation()}
      onMouseDown={(event) => event.stopPropagation()}
      onClick={(event) => {
        event.preventDefault();
        event.stopPropagation();
        if (!unavailable) onOpenFilePreview(row.path);
      }}
    >
      <VsIcon name="openFile" size={13} />
    </span>
  );
}

function ChangeFileRow({
  row,
  treePath,
  displayName,
  depth,
  selected,
  cwd,
  showStatus,
  onOpenFile,
  onOpenFilePreview,
  guardDeletedPreview,
}) {
  const tree = Number.isInteger(depth);
  const pathParts = splitPath(row.path);
  const canReveal = guardDeletedPreview ? row.status !== 'D' : undefined;
  const additions = Number.isFinite(row.additions) ? String(row.additions) : undefined;
  const deletions = Number.isFinite(row.deletions) ? String(row.deletions) : undefined;
  const absolutePath = cwd ? joinWorkspacePath(cwd, row.path) : undefined;

  return (
    <button
      type="button"
      role={tree ? 'treeitem' : undefined}
      aria-level={tree ? depth + 1 : undefined}
      data-change-compact-file={row.path}
      data-change-tree-path={treePath || normalizeChangeTreePath(row.path, cwd)}
      data-change-tree-depth={tree ? depth : undefined}
      data-desktop-review-kind="file"
      data-desktop-review-file={row.path || undefined}
      data-desktop-review-absolute-path={absolutePath}
      data-desktop-review-additions={additions}
      data-desktop-review-deletions={deletions}
      data-desktop-review-can-reveal={canReveal == null ? undefined : String(canReveal)}
      className={clsx(
        'ace-change-compact-row',
        tree && 'ace-change-tree-file-row',
        selected && 'is-selected',
      )}
      onClick={() => onOpenFile?.(row.path)}
      title={row.path || treePath || displayName}
    >
      {tree ? (
        <span
          className="ace-change-tree-entry"
          style={{ '--ace-change-tree-depth': Math.min(depth, 8) }}
        >
          <span className="ace-change-tree-arrow-spacer" aria-hidden="true" />
          <FileMarker row={row} showStatus={showStatus} tree />
          <span className="ace-change-tree-name">{displayName || pathParts.name || treePath}</span>
        </span>
      ) : (
        <>
          <FileMarker row={row} showStatus={showStatus} tree={false} />
          <span className="ace-change-compact-file">
            <span className="ace-change-compact-name">{pathParts.name}</span>
            {pathParts.parent && (
              <span className="ace-change-compact-parent">{pathParts.parent}</span>
            )}
          </span>
        </>
      )}
      <ChangeCounts row={row} />
      <OpenPreviewAction
        row={row}
        onOpenFilePreview={onOpenFilePreview}
        guardDeletedPreview={guardDeletedPreview}
      />
    </button>
  );
}

function ChangeDirectoryName({ node }) {
  const segments = Array.isArray(node.segments) && node.segments.length > 0
    ? node.segments
    : [node.name];
  if (segments.length === 1) {
    return <span className="ace-change-tree-name">{segments[0]}</span>;
  }

  const suffix = segments[segments.length - 1];
  const prefix = segments.slice(0, -1).join('/');
  return (
    <span className="ace-change-tree-name ace-change-tree-compact-name">
      <span className="ace-change-tree-path-prefix">{prefix}</span>
      <span className="ace-change-tree-path-separator" aria-hidden="true">/</span>
      <span className="ace-change-tree-path-suffix">{suffix}</span>
    </span>
  );
}

function ChangeDirectoryRow({ node, depth, collapsed, onToggle }) {
  return (
    <button
      type="button"
      role="treeitem"
      aria-level={depth + 1}
      aria-expanded={!collapsed}
      data-change-tree-directory={node.path}
      data-change-tree-depth={depth}
      className="ace-change-tree-directory-row"
      onClick={() => onToggle(node.path)}
      title={node.path}
      aria-label={node.path}
    >
      <span
        className="ace-change-tree-entry"
        style={{ '--ace-change-tree-depth': Math.min(depth, 8) }}
      >
        <VsIcon name={collapsed ? 'expandRight' : 'glyphDown'} size={10} />
        <VsIcon name={collapsed ? 'folder' : 'folderOpen'} size={15} />
        <ChangeDirectoryName node={node} />
      </span>
    </button>
  );
}

function changePathsMatch(originalPath, treePath, selectedPath, selectedTreePath) {
  const original = normalizeTreePath(originalPath);
  const display = normalizeTreePath(treePath);
  return (!!selectedPath && original === selectedPath)
    || (!!selectedTreePath && display === selectedTreePath);
}

function selectedRowMatches(node, selectedPath, selectedTreePath) {
  return changePathsMatch(
    node.getAttribute('data-change-compact-file'),
    node.getAttribute('data-change-tree-path'),
    selectedPath,
    selectedTreePath,
  );
}

export function ChangeFileList({
  rows,
  viewMode,
  cwd = '',
  selectedFile = '',
  selectedFileRevision = 0,
  showStatus = false,
  onOpenFile,
  onOpenFilePreview,
  guardDeletedPreview = false,
}) {
  const list = Array.isArray(rows) ? rows : [];
  const treeMode = viewMode === CHANGE_LIST_VIEW_TREE;
  const listRef = useRef(null);
  const [collapsedPaths, setCollapsedPaths] = useState(() => new Set());
  const selectedPath = normalizeTreePath(selectedFile);
  const selectedTreePath = normalizeChangeTreePath(selectedFile, cwd);
  const tree = useMemo(() => buildChangeFileTree(list, cwd), [list, cwd]);
  const visibleTree = useMemo(
    () => flattenVisibleChangeTree(tree, collapsedPaths),
    [tree, collapsedPaths],
  );

  useEffect(() => {
    if (!treeMode || !selectedFile) return;
    const ancestors = changeTreeAncestorPaths(selectedFile, cwd);
    if (ancestors.length === 0) return;
    setCollapsedPaths((previous) => {
      if (!ancestors.some((path) => previous.has(path))) return previous;
      const next = new Set(previous);
      for (const path of ancestors) next.delete(path);
      return next;
    });
  }, [cwd, selectedFile, selectedFileRevision, treeMode]);

  useLayoutEffect(() => {
    if (!selectedPath && !selectedTreePath) return;
    const element = listRef.current;
    if (!element) return;
    const row = Array.from(element.querySelectorAll('[data-change-compact-file]'))
      .find((candidate) => selectedRowMatches(candidate, selectedPath, selectedTreePath));
    if (!row) return;
    const listRect = element.getBoundingClientRect();
    const rowRect = row.getBoundingClientRect();
    if (rowRect.top < listRect.top) {
      element.scrollTop = Math.max(0, element.scrollTop + rowRect.top - listRect.top);
    } else if (rowRect.bottom > listRect.bottom) {
      element.scrollTop = Math.max(0, element.scrollTop + rowRect.bottom - listRect.bottom);
    }
  }, [collapsedPaths, selectedFileRevision, selectedPath, selectedTreePath, treeMode]);

  const toggleDirectory = (path) => {
    setCollapsedPaths((previous) => {
      const next = new Set(previous);
      if (next.has(path)) next.delete(path);
      else next.add(path);
      return next;
    });
  };

  return (
    <div
      className={clsx('ace-change-compact-list', treeMode && 'ace-change-tree-list')}
      ref={listRef}
      role={treeMode ? 'tree' : undefined}
      aria-label={treeMode ? '变更文件目录树' : undefined}
    >
      {treeMode ? visibleTree.map(({ node, depth }) => (
        node.type === 'directory' ? (
          <ChangeDirectoryRow
            key={node.key}
            node={node}
            depth={depth}
            collapsed={collapsedPaths.has(node.path)}
            onToggle={toggleDirectory}
          />
        ) : (
          <ChangeFileRow
            key={node.key}
            row={node.row}
            treePath={node.treePath}
            displayName={node.name}
            depth={depth}
            selected={changePathsMatch(
              node.row.path,
              node.treePath,
              selectedPath,
              selectedTreePath,
            )}
            cwd={cwd}
            showStatus={showStatus}
            onOpenFile={onOpenFile}
            onOpenFilePreview={onOpenFilePreview}
            guardDeletedPreview={guardDeletedPreview}
          />
        )
      )) : list.map((row, index) => (
        <ChangeFileRow
          key={`${row.path}:${index}`}
          row={row}
          selected={changePathsMatch(
            row.path,
            normalizeChangeTreePath(row.path, cwd),
            selectedPath,
            selectedTreePath,
          )}
          cwd={cwd}
          showStatus={showStatus}
          onOpenFile={onOpenFile}
          onOpenFilePreview={onOpenFilePreview}
          guardDeletedPreview={guardDeletedPreview}
        />
      ))}
    </div>
  );
}
