import { isDismissShortcut } from '../lib/shortcutBindings.js';
// Web 路径选择器弹窗(openspec add-web-path-picker):浏览 daemon 所在机器的文件系统,
// 选一个目录(mode='folder')或文件(mode='file')。数据来自 /api/fs/roots 与 /api/fs/list,
// 纯逻辑在 lib/pathPicker.js,Promise 形态的入口在 lib/pathPickerHost.js。
//
// 只有没有 Desktop bridge 的页面(普通浏览器、Edge --app 兼容模式、远程 Web)会走到这里,
// Desktop 壳仍用原生对话框。返回的路径保持用户浏览时的写法(junction 不解析),与原生
// 对话框一致,同一目录才会注册成同一个工作区。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { clsx, formatBytes, formatDateTime } from '../lib/format.js';
import {
  PICKER_MODE_FILE,
  browseErrorKey,
  initialPickerPath,
  isSelectableEntry,
  normalizePickerPath,
  pickerBreadcrumbs,
  pickerParentPath,
  pickerSelectionTarget,
  rootDisplayName,
  rootUsagePercent,
  selectableEntries,
  sortPickerEntries,
  typeaheadMatch,
} from '../lib/pathPicker.js';

const TYPEAHEAD_RESET_MS = 800;

function subtitleFor(mode, purpose) {
  const fileMode = mode === PICKER_MODE_FILE;
  if (purpose === 'workspace') return '选中的目录会作为新的工作区添加到侧栏。';
  if (purpose === 'preview') return '选中的文件会在侧面板里打开预览。';
  if (purpose === 'settings') return fileMode ? '选中的文件会写入这项设置。' : '选中的目录会写入这项设置。';
  return fileMode ? '在服务端文件系统里选择一个文件。' : '在服务端文件系统里选择一个目录。';
}

function quickLabel(kind) {
  if (kind === 'home') return '主目录';
  if (kind === 'desktop') return '桌面';
  if (kind === 'projects') return '工作区目录';
  return kind;
}

function browseErrorText(error, path) {
  const key = browseErrorKey(error?.status);
  const detail = error?.body?.detail || error?.message || '';
  if (key === 'permission-denied') return `无权限读取这个目录:${path}`;
  if (key === 'not-found') return `找不到路径:${path}`;
  if (key === 'not-absolute') return '请输入完整的绝对路径,例如 C:\\Users 或 /home';
  return `读取目录失败:${detail || path}`;
}

function primaryLabelFor(mode, target) {
  if (mode === PICKER_MODE_FILE) return '打开文件';
  if (target?.label === 'select-named') return `选择「${target.name}」`;
  return '选择当前文件夹';
}

function IconButton({ label, onClick, disabled, children }) {
  return (
    <button
      type="button"
      className="ace-path-picker-icon-btn"
      onClick={onClick}
      disabled={disabled}
      aria-label={label}
      title={label}
    >
      {children}
    </button>
  );
}

export function PathPickerModal({
  api,
  mode = 'folder',
  initialPath = '',
  lastFolder = '',
  purpose = '',
  onResolve,
}) {
  const isFileMode = mode === PICKER_MODE_FILE;
  const [roots, setRoots] = useState(null);
  const [currentPath, setCurrentPath] = useState(null); // null = 尚未初始化;'' = 根节点视图
  const [listing, setListing] = useState({ path: '', parent: '', entries: [], truncated: false });
  const [loading, setLoading] = useState(false);
  const [listError, setListError] = useState('');
  const [selected, setSelected] = useState(null);
  const [showHidden, setShowHidden] = useState(false);
  const [editing, setEditing] = useState(false);
  const [pathDraft, setPathDraft] = useState('');
  const [historyLength, setHistoryLength] = useState(0);

  const historyRef = useRef([]);
  const currentPathRef = useRef(null);
  const showHiddenRef = useRef(false);
  const requestSeqRef = useRef(0);
  const pendingSelectNameRef = useRef('');
  const listRef = useRef(null);
  const inputRef = useRef(null);
  const typeaheadRef = useRef({ buffer: '', timer: 0 });
  const resolvedRef = useRef(false);

  currentPathRef.current = currentPath;
  showHiddenRef.current = showHidden;

  const finish = useCallback((result) => {
    if (resolvedRef.current) return;
    resolvedRef.current = true;
    onResolve?.(result);
  }, [onResolve]);
  const cancel = useCallback(() => finish(null), [finish]);

  const focusList = useCallback(() => {
    requestAnimationFrame(() => listRef.current?.focus());
  }, []);

  // 导航:'' = 根节点视图;其它值先归一再请求。失败时停留在原目录并给内联提示。
  const navigate = useCallback(async (path, { pushHistory = true, keepError = false } = {}) => {
    const target = path === '' ? '' : normalizePickerPath(path);
    if (path !== '' && !target) {
      setListError('请输入完整的绝对路径,例如 C:\\Users 或 /home');
      return false;
    }
    const seq = ++requestSeqRef.current;
    const previous = currentPathRef.current;
    if (target === '') {
      if (pushHistory && previous !== null && previous !== '') historyRef.current.push(previous);
      setHistoryLength(historyRef.current.length);
      setListing({ path: '', parent: '', entries: [], truncated: false });
      setCurrentPath('');
      setSelected(null);
      if (!keepError) setListError('');
      setEditing(false);
      return true;
    }
    setLoading(true);
    try {
      const result = await api.fsList(target, { showHidden: showHiddenRef.current });
      if (seq !== requestSeqRef.current) return false;
      if (pushHistory && previous !== null && previous !== result.path) historyRef.current.push(previous);
      setHistoryLength(historyRef.current.length);
      setListing({
        path: result.path || target,
        parent: typeof result.parent === 'string' ? result.parent : pickerParentPath(target),
        entries: Array.isArray(result.entries) ? result.entries : [],
        truncated: !!result.truncated,
      });
      setCurrentPath(result.path || target);
      const wanted = pendingSelectNameRef.current;
      pendingSelectNameRef.current = '';
      const entries = Array.isArray(result.entries) ? result.entries : [];
      setSelected(wanted ? entries.find((entry) => entry.name === wanted) || null : null);
      setListError('');
      setEditing(false);
      return true;
    } catch (error) {
      if (seq !== requestSeqRef.current) return false;
      setListError(browseErrorText(error, target));
      return false;
    } finally {
      if (seq === requestSeqRef.current) setLoading(false);
    }
  }, [api]);

  // 打开:先取根节点,再按「调用方目录 > 上次目录 > 主目录 > 根视图」决定起点。
  useEffect(() => {
    let cancelled = false;
    (async () => {
      let home = '';
      try {
        const result = await api.fsRoots();
        if (cancelled) return;
        setRoots(result || null);
        home = result?.home || '';
      } catch (error) {
        if (cancelled) return;
        setRoots({ host: '', os: '', home: '', roots: [], quick: [], workspaces: [] });
        setListError(`读取根目录失败:${error?.message || ''}`);
      }
      const start = initialPickerPath({ mode, initialPath, lastFolder, home });
      const ok = await navigate(start, { pushHistory: false, keepError: true });
      if (!cancelled && !ok && start !== '') {
        await navigate(home || '', { pushHistory: false, keepError: true });
      }
    })();
    return () => { cancelled = true; };
    // 只在打开时跑一次;调用方的 initialPath / lastFolder 不会在弹窗生命周期内变化。
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 隐藏开关变化后重读当前目录(根视图没有隐藏概念)。
  useEffect(() => {
    if (currentPathRef.current) void navigate(currentPathRef.current, { pushHistory: false });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [showHidden]);

  useEffect(() => {
    if (currentPath !== null && !editing) focusList();
  }, [currentPath, editing, focusList]);

  useEffect(() => () => clearTimeout(typeaheadRef.current.timer), []);

  const goUp = useCallback(() => {
    const current = currentPathRef.current;
    if (!current) return;
    const parent = listing.parent || pickerParentPath(current);
    const slash = current.lastIndexOf('/');
    pendingSelectNameRef.current = slash >= 0 ? current.slice(slash + 1) : '';
    void navigate(parent || '');
  }, [listing.parent, navigate]);

  const goBack = useCallback(() => {
    if (historyRef.current.length === 0) return;
    const previous = historyRef.current.pop();
    setHistoryLength(historyRef.current.length);
    void navigate(previous, { pushHistory: false });
  }, [navigate]);

  const reload = useCallback(() => {
    const current = currentPathRef.current;
    if (current) void navigate(current, { pushHistory: false });
  }, [navigate]);

  const rootRows = useMemo(() => (roots?.roots || []).map((root) => ({
    name: rootDisplayName(root, '本地磁盘'),
    path: normalizePickerPath(root.path),
    kind: 'dir',
    isRoot: true,
    usage: rootUsagePercent(root),
    free_bytes: root.free_bytes,
  })), [roots]);

  const rows = useMemo(() => (
    currentPath === '' ? rootRows : sortPickerEntries(listing.entries)
  ), [currentPath, rootRows, listing.entries]);

  const target = useMemo(() => pickerSelectionTarget({ mode, currentPath: currentPath || '', selected }), [mode, currentPath, selected]);
  const primaryLabel = primaryLabelFor(mode, target);

  const confirm = useCallback(() => {
    if (!target) return;
    finish({ path: target.path, kind: target.kind });
  }, [finish, target]);

  const activateEntry = useCallback((entry) => {
    if (!entry) return;
    if (entry.kind === 'dir') {
      void navigate(entry.path);
      return;
    }
    if (isFileMode && entry.kind === 'file') {
      finish({ path: entry.path, kind: 'file' });
    }
  }, [finish, isFileMode, navigate]);

  const selectEntry = useCallback((entry) => {
    if (!isSelectableEntry(entry, mode)) return;
    setSelected(entry);
    requestAnimationFrame(() => {
      listRef.current?.querySelector('[aria-selected="true"]')?.scrollIntoView?.({ block: 'nearest' });
    });
  }, [mode]);

  const startEdit = useCallback(() => {
    setPathDraft(currentPathRef.current || '');
    setEditing(true);
    requestAnimationFrame(() => {
      inputRef.current?.focus();
      inputRef.current?.select();
    });
  }, []);

  const stopEdit = useCallback(() => {
    setEditing(false);
    focusList();
  }, [focusList]);

  const submitPath = useCallback(async () => {
    const ok = await navigate(pathDraft.trim() === '' ? '' : pathDraft);
    if (ok) focusList();
  }, [focusList, navigate, pathDraft]);

  const onListKeyDown = useCallback((event) => {
    const candidates = selectableEntries(rows, mode);
    const index = selected ? candidates.findIndex((entry) => entry.path === selected.path) : -1;
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      if (candidates.length === 0) return;
      const next = event.key === 'ArrowDown'
        ? Math.min(candidates.length - 1, index + 1)
        : Math.max(0, index - 1);
      selectEntry(candidates[next]);
      return;
    }
    if (event.key === 'Home' || event.key === 'End') {
      event.preventDefault();
      if (candidates.length === 0) return;
      selectEntry(candidates[event.key === 'Home' ? 0 : candidates.length - 1]);
      return;
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      if (event.ctrlKey || event.metaKey) {
        confirm();
        return;
      }
      if (!selected) {
        if (!isFileMode) confirm();
        return;
      }
      activateEntry(selected);
      return;
    }
    if (event.key === 'Backspace') {
      event.preventDefault();
      goUp();
      return;
    }
    if ((event.key === '/' || event.key === '\\') && !event.ctrlKey && !event.metaKey && !event.altKey) {
      event.preventDefault();
      startEdit();
      return;
    }
    if (event.key.length === 1 && !event.ctrlKey && !event.metaKey && !event.altKey) {
      const state = typeaheadRef.current;
      clearTimeout(state.timer);
      state.buffer += event.key.toLowerCase();
      state.timer = setTimeout(() => { state.buffer = ''; }, TYPEAHEAD_RESET_MS);
      const match = typeaheadMatch(candidates, state.buffer);
      if (match) selectEntry(match);
    }
  }, [activateEntry, confirm, goUp, isFileMode, mode, rows, selectEntry, selected, startEdit]);

  // 浏览器会把 Ctrl+L / Alt+D 抢去聚焦地址栏,页面根本收不到;所以路径输入用文件管理器
  // 的习惯:在列表里按 / 或 \ 直接进入路径编辑(Nautilus 同款),点面包屑空白处也可以。
  const onDialogKeyDown = useCallback((event) => {
    if (event.altKey && event.key === 'ArrowLeft') {
      event.preventDefault();
      goBack();
    }
  }, [goBack]);

  const onInputKeyDown = useCallback((event) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      void submitPath();
    } else if (isDismissShortcut(event)) {
      event.preventDefault();
      event.stopPropagation();
      stopEdit();
    }
  }, [stopEdit, submitPath]);

  const crumbs = useMemo(() => pickerBreadcrumbs(currentPath || ''), [currentPath]);
  const quickItems = useMemo(() => [
    { key: 'roots', label: '此电脑', path: '', icon: 'computer' },
    ...(roots?.quick || []).map((item) => ({
      key: item.kind,
      label: quickLabel(item.kind),
      path: normalizePickerPath(item.path),
      icon: 'folder',
    })),
  ], [roots]);
  const workspaceItems = useMemo(() => (roots?.workspaces || []).map((item) => ({
    key: item.hash || item.path,
    label: item.name || item.path,
    path: normalizePickerPath(item.path),
    icon: 'folderOpen',
  })), [roots]);

  const isActive = (path) => currentPath !== null && path === currentPath;
  const emptyText = currentPath === ''
    ? (roots ? '没有可浏览的根目录' : '正在读取…')
    : (loading ? '正在读取…' : '这个目录没有可显示的项目');

  return (
    <Modal onClose={cancel} width={860} dismissOnBackdrop={false} labelledBy="ace-path-picker-title">
      {() => (
        <div className="ace-path-picker" onKeyDown={onDialogKeyDown}>
          <div className="shrink-0 px-[18px] pt-3.5 pb-3 border-b border-border flex items-start gap-3">
            <div className="w-9 h-9 rounded-lg bg-accent/10 text-accent flex items-center justify-center shrink-0">
              <VsIcon name={isFileMode ? 'file' : 'folderOpen'} size={19} />
            </div>
            <div className="min-w-0 flex-1">
              <h2 id="ace-path-picker-title" className="text-[15px] font-semibold text-fg">
                {isFileMode ? '打开文件' : '选择文件夹'}
              </h2>
              <p className="mt-0.5 text-[12px] leading-5 text-fg-mute">{subtitleFor(mode, purpose)}</p>
            </div>
            <span className="ace-path-picker-host" title="路径都是 daemon 所在机器上的路径">
              <i />
              {roots?.host ? `${roots.host} · ` : ''}服务端文件系统
            </span>
            <IconButton label="关闭" onClick={cancel}><VsIcon name="close" size={15} /></IconButton>
          </div>

          <div className="shrink-0 flex items-center gap-1.5 px-3 py-2 border-b border-border">
            <IconButton label="后退" onClick={goBack} disabled={historyLength === 0}>
              <VsIcon name="arrowLeft" size={15} />
            </IconButton>
            <IconButton label="上一级" onClick={goUp} disabled={!currentPath}>
              <VsIcon name="expandUp" size={15} />
            </IconButton>
            {editing ? (
              <input
                ref={inputRef}
                value={pathDraft}
                onChange={(event) => setPathDraft(event.target.value)}
                onKeyDown={onInputKeyDown}
                onBlur={stopEdit}
                spellCheck={false}
                autoComplete="off"
                placeholder="输入或粘贴绝对路径,回车跳转"
                className="ace-path-picker-input font-mono"
                aria-label="路径"
              />
            ) : (
              <div
                className="ace-path-picker-crumbs"
                title="点击空白处或按 / 直接输入路径"
                onClick={(event) => {
                  if (event.target.closest('[data-crumb]')) return;
                  startEdit();
                }}
              >
                <button
                  type="button"
                  data-crumb="root"
                  className={clsx('ace-path-picker-crumb', currentPath === '' && 'is-current')}
                  onClick={() => void navigate('')}
                >
                  <VsIcon name="computer" size={13} />
                  此电脑
                </button>
                {crumbs.map((crumb, index) => (
                  <span key={crumb.path} className="flex items-center gap-0.5 min-w-0">
                    <VsIcon name="expandRight" size={12} className="text-fg-mute shrink-0" />
                    <button
                      type="button"
                      data-crumb={crumb.path}
                      className={clsx('ace-path-picker-crumb', index === crumbs.length - 1 && 'is-current')}
                      onClick={() => void navigate(crumb.path)}
                    >
                      {crumb.label}
                    </button>
                  </span>
                ))}
                <span className="flex-1" />
                <VsIcon name="edit" size={13} className="ace-path-picker-pen" />
              </div>
            )}
            <button
              type="button"
              className="ace-path-picker-toggle"
              aria-pressed={showHidden}
              onClick={() => setShowHidden((value) => !value)}
            >
              <span className="ace-path-picker-switch" />
              显示隐藏
            </button>
            <IconButton label="刷新" onClick={reload} disabled={!currentPath}>
              <VsIcon name="refresh" size={14} />
            </IconButton>
          </div>

          {listError && (
            <div role="alert" className="ace-path-picker-error">{listError}</div>
          )}

          <div className="flex-1 min-h-0 flex">
            <nav className="ace-path-picker-rail ace-scrollbar" aria-label="快捷入口">
              <div className="ace-path-picker-rail-sec">快捷入口</div>
              {quickItems.map((item) => (
                <button
                  key={item.key}
                  type="button"
                  className={clsx('ace-path-picker-rail-item', isActive(item.path) && 'is-active')}
                  onClick={() => void navigate(item.path)}
                  title={item.path || undefined}
                >
                  <VsIcon name={item.icon} size={15} />
                  <span className="truncate">{item.label}</span>
                </button>
              ))}
              {workspaceItems.length > 0 && (
                <>
                  <div className="ace-path-picker-rail-sec">已注册工作区</div>
                  {workspaceItems.map((item) => (
                    <button
                      key={item.key}
                      type="button"
                      className={clsx('ace-path-picker-rail-item', isActive(item.path) && 'is-active')}
                      onClick={() => void navigate(item.path)}
                      title={item.path}
                    >
                      <VsIcon name={item.icon} size={15} />
                      <span className="truncate">{item.label}</span>
                    </button>
                  ))}
                </>
              )}
            </nav>

            <div className="flex-1 min-w-0 flex flex-col">
              <div className="ace-path-picker-cols">
                <span />
                <span>名称</span>
                <span className="ace-path-picker-mtime">修改时间</span>
                <span className="text-right">大小</span>
              </div>
              <div
                ref={listRef}
                className="ace-path-picker-list ace-scrollbar"
                role="listbox"
                tabIndex={0}
                aria-label="目录内容"
                aria-busy={loading}
                onKeyDown={onListKeyDown}
              >
                {rows.length === 0 ? (
                  <div className="ace-path-picker-empty">
                    {emptyText}
                    {!loading && currentPath && !showHidden && (
                      <button type="button" className="ace-path-picker-link" onClick={() => setShowHidden(true)}>
                        显示隐藏项目
                      </button>
                    )}
                  </div>
                ) : rows.map((row) => {
                  const selectable = isSelectableEntry(row, mode);
                  const isSelected = !!selected && selected.path === row.path;
                  return (
                    <div
                      key={row.path}
                      role="option"
                      aria-selected={isSelected}
                      data-dim={selectable ? undefined : 'true'}
                      data-hidden={row.hidden ? 'true' : undefined}
                      className="ace-path-picker-row"
                      title={row.path}
                      onClick={() => selectEntry(row)}
                      onDoubleClick={() => activateEntry(row)}
                    >
                      <span className={clsx('ace-path-picker-ic', row.kind === 'dir' && !row.isRoot && 'is-dir')}>
                        <VsIcon name={row.isRoot ? 'computer' : row.kind === 'dir' ? 'folder' : 'file'} size={15} />
                      </span>
                      <span className="min-w-0 flex items-center gap-2 overflow-hidden">
                        <span className="truncate">{row.name}</span>
                        {row.link_target && (
                          <>
                            <span className="ace-path-picker-badge" title="目录链接">链接</span>
                            <span className="font-mono text-[11px] text-fg-mute truncate">→ {row.link_target}</span>
                          </>
                        )}
                        {row.hidden && <span className="ace-path-picker-badge">隐藏</span>}
                      </span>
                      <span className="ace-path-picker-mtime ace-path-picker-meta">
                        {row.isRoot
                          ? (row.usage != null && (
                            <span className="ace-path-picker-usage" aria-hidden="true">
                              <i style={{ width: `${row.usage}%` }} />
                            </span>
                          ))
                          : (row.modified_ms ? formatDateTime(row.modified_ms) : '')}
                      </span>
                      <span className="ace-path-picker-meta text-right">
                        {row.isRoot
                          ? (row.free_bytes != null ? `${formatBytes(row.free_bytes)} 可用` : '')
                          : (row.kind === 'file' ? formatBytes(row.size || 0) : '—')}
                      </span>
                    </div>
                  );
                })}
                {listing.truncated && currentPath !== '' && (
                  <div className="ace-path-picker-empty">目录项目过多,只显示前 {rows.length} 项</div>
                )}
              </div>
            </div>
          </div>

          <div className="shrink-0 px-3.5 py-2.5 border-t border-border bg-surface-alt flex items-center gap-2.5">
            <div className="flex-1 min-w-0 flex items-center gap-2 text-[12px] text-fg-mute">
              {target ? (
                <>
                  <span className="shrink-0">{isFileMode ? '将打开' : '将选择'}</span>
                  <span className="font-mono text-[11.5px] text-fg truncate" title={target.path}>{target.path}</span>
                </>
              ) : (
                <span>{isFileMode ? '在列表中选择一个文件' : '进入一个磁盘或目录后再选择'}</span>
              )}
            </div>
            <button
              type="button"
              onClick={cancel}
              className="h-8 px-3 rounded-lg text-[12px] text-fg hover:bg-surface-hi"
            >
              取消
            </button>
            <button
              type="button"
              data-ace-dialog-primary="true"
              onClick={confirm}
              disabled={!target}
              className="h-8 min-w-[108px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 disabled:hover:brightness-100"
            >
              {primaryLabel}
            </button>
          </div>
        </div>
      )}
    </Modal>
  );
}
