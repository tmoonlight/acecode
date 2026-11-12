// 摘要面板显示工作区、产物、变更和后台任务。
//
// 收起、停靠和悬浮由外层的 ResponsiveSidebar 负责,这里只渲染
// 内容并按需取数:
//   - 分支走共享的 gitInfoCache(30s TTL + 在途去重),与会话 git 胶囊同一份;
//   - 产物(这次对话生成的文档 / 表格 / 图片)取 /api/sessions/:id/outputs,
//     面板可见时才取;回合进行中隔几秒看一眼,回合结束再取一次;
//   - 变更合计与右侧「变更」页共用 sessionChangesCache 和同一个基线(workbench
//     状态 gitBase:<cwd>),只在面板可见时拉取,回合结束 / checkout 后标脏重拉;
//   - 后台任务直接用 ChatView 已有的子代理任务列表,不另开订阅。
import { useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { createPortal } from 'react-dom';
import { clsx } from '../lib/format.js';
import {
  CONTEXT_MENU_DELEGATE_EVENT,
  DESKTOP_CONTEXT_ACTIONS,
  openDesktopContextMenu,
} from '../lib/desktopContextMenu.js';
import { buildBaseCandidates } from '../lib/gitChanges.js';
import { sessionChangesCache } from '../lib/gitChangesCache.js';
import { gitInfoCache } from '../lib/gitInfoCache.js';
import { GIT_STATE_CHANGED_EVENT } from '../lib/gitSessionPill.js';
import {
  normalizeSessionOutputs,
  sessionOutputRows,
  sessionOutputsEqual,
} from '../lib/sessionOutputs.js';
import {
  folderLabel,
  formatChangeCount,
  summaryChangeTotals,
  summaryFinishedTaskCount,
  summaryRunningTasks,
  summaryWorkspaceModel,
} from '../lib/summaryPanel.js';
import { useWorkbenchState } from '../lib/useWorkbenchState.js';
import { subscribeWorkspaceDirectory, workspaceDirectoryEntry } from '../lib/workspaceDirectory.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';
import { SubagentAvatar } from './PixelAvatar.jsx';
import { WorkspaceIcon } from './WorkspaceIcon.jsx';

const SUMMARY_FOLDER_CONTEXT_MENU_DELEGATE = 'summary-folder';
const OUTPUTS_BUSY_POLL_MS = 6000;
const NO_OUTPUTS = [];

// 产物列表。回合进行中脚本随时可能生成文件,而界面收不到「bash 写了文件」的
// 事件,所以面板可见且回合在跑时隔几秒取一次;内容没变就留着旧数组不重渲染。
function useSessionOutputs({ api, sessionId, workspaceHash, busy, enabled }) {
  const [outputs, setOutputs] = useState(NO_OUTPUTS);
  const sessionRef = useRef(sessionId);
  sessionRef.current = sessionId;
  const load = useCallback(() => {
    const target = sessionId;
    if (!target || typeof api?.sessionOutputs !== 'function') return;
    api.sessionOutputs(target, workspaceHash)
      .then((data) => {
        if (sessionRef.current !== target) return;
        const next = normalizeSessionOutputs(data);
        setOutputs((previous) => (sessionOutputsEqual(previous, next) ? previous : next));
      })
      .catch(() => {});
  }, [api, sessionId, workspaceHash]);

  // 换会话先清空,别把上一个会话的文件挂在新会话下面。
  useEffect(() => {
    setOutputs((previous) => (previous.length ? NO_OUTPUTS : previous));
  }, [sessionId]);

  useEffect(() => {
    if (!enabled || !sessionId) return undefined;
    load();
    if (!busy) return undefined;
    const timer = window.setInterval(load, OUTPUTS_BUSY_POLL_MS);
    return () => window.clearInterval(timer);
  }, [busy, enabled, sessionId, load]);

  return sessionId ? outputs : NO_OUTPUTS;
}

function useGitInfo(api, cwd, owner, enabled) {
  const [gitInfo, setGitInfo] = useState(() => gitInfoCache.peek(api, cwd, owner) ?? null);
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;
  const load = useCallback((force = false) => {
    const target = cwdRef.current;
    if (!target) { setGitInfo(null); return; }
    const request = force ? gitInfoCache.refresh(api, target, owner) : gitInfoCache.get(api, target, owner);
    request
      .then((info) => { if (cwdRef.current === target) setGitInfo(info || null); })
      .catch(() => { if (cwdRef.current === target) setGitInfo(null); });
  }, [api, owner]);
  useEffect(() => {
    setGitInfo(gitInfoCache.peek(api, cwd, owner) ?? null);
    if (enabled && cwd) load(false);
  }, [api, cwd, owner, enabled, load]);
  useEffect(() => {
    if (!enabled) return undefined;
    const handler = (event) => {
      const changed = String(event?.detail?.cwd || '');
      if (changed && changed !== cwdRef.current) return;
      load(false);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [enabled, load]);
  return gitInfo;
}

function useGitChangeList({ api, owner, cwd, gitInfo, busy, enabled }) {
  const isRepo = !!gitInfo?.is_repo;
  const { initial } = useMemo(() => buildBaseCandidates(gitInfo), [gitInfo]);
  const [base] = useWorkbenchState(owner, `gitBase:${cwd}`, initial);
  const cache = useMemo(() => sessionChangesCache(owner), [owner]);
  const [list, setList] = useState(() => (cwd && base ? cache.getListEvenIfStale(cwd, base) : null));
  const keyRef = useRef('');
  keyRef.current = `${cwd}\u0000${base}`;

  const fetchList = useCallback((force = false) => {
    if (!cwd || !base) return;
    if (!force) {
      const cached = cache.getList(cwd, base);
      if (cached) { setList(cached); return; }
    }
    const key = `${cwd}\u0000${base}`;
    setList(cache.getListEvenIfStale(cwd, base));
    api.gitChanges(cwd, base)
      .then((data) => {
        cache.putList(cwd, base, data);
        if (keyRef.current === key) setList(data);
      })
      .catch(() => {});
  }, [api, base, cache, cwd]);

  useEffect(() => {
    if (!enabled || !isRepo) return;
    fetchList(false);
  }, [enabled, isRepo, fetchList]);

  // 回合结束(busy true → false)工作树可能变了:标脏,可见时重拉。
  const wasBusy = useRef(busy);
  useEffect(() => {
    const previous = wasBusy.current;
    wasBusy.current = busy;
    if (!previous || busy || !cwd) return;
    cache.markStale(cwd);
    if (enabled && isRepo) fetchList(true);
  }, [busy, cache, cwd, enabled, isRepo, fetchList]);

  useEffect(() => {
    if (!enabled || !isRepo) return undefined;
    const handler = (event) => {
      const changed = String(event?.detail?.cwd || '');
      if (changed && changed !== cwd) return;
      fetchList(true);
    };
    window.addEventListener(GIT_STATE_CHANGED_EVENT, handler);
    return () => window.removeEventListener(GIT_STATE_CHANGED_EVENT, handler);
  }, [cwd, enabled, isRepo, fetchList]);

  return isRepo ? list : null;
}

function useNow(ticking) {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!ticking) return undefined;
    setNow(Date.now());
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [ticking]);
  return now;
}

// 分组标题可折叠的带 ⌄ / › 箭头(照 Claude 桌面版);字号沿用原来的,随设置里的字体大小缩放。
// 折叠状态按分组记在本机(localStorage),读写失败就当展开。
function Section({ id = '', title, action = null, collapsible = false, children }) {
  const storageKey = collapsible && id ? `acecode.summaryPanel.collapsed.${id}` : '';
  const [collapsed, setCollapsed] = useState(() => {
    if (!storageKey) return false;
    try { return globalThis.localStorage?.getItem(storageKey) === '1'; } catch { return false; }
  });
  const toggle = () => setCollapsed((value) => {
    const next = !value;
    try { globalThis.localStorage?.setItem(storageKey, next ? '1' : '0'); } catch { /* 只是本机偏好 */ }
    return next;
  });
  return (
    <section className="ace-summary-section" data-collapsed={collapsed ? 'true' : undefined}>
      <div className="flex items-center gap-1 h-8 pl-2 min-w-0">
        <h3 className="flex-1 min-w-0 flex text-[12px] font-semibold text-fg-2">
          {collapsible ? (
            <button
              type="button"
              onClick={toggle}
              aria-expanded={!collapsed}
              className="min-w-0 flex items-center gap-1 rounded-md text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
            >
              <span className="truncate">{title}</span>
              <VsIcon name={collapsed ? 'expandRight' : 'expandDown'} size={12} className="shrink-0 text-fg-mute" />
            </button>
          ) : (
            <span className="truncate">{title}</span>
          )}
        </h3>
        {action}
      </div>
      {!collapsed && children}
    </section>
  );
}

function Row({ icon, iconNode = null, children, title, onClick, trailing }) {
  const content = (
    <>
      {iconNode || <VsIcon name={icon} size={15} className="shrink-0 text-fg-mute" />}
      <span className="flex-1 min-w-0 truncate">{children}</span>
    </>
  );
  return (
    <div className="ace-summary-row group flex items-center gap-1 min-w-0">
      {onClick ? (
        <button
          type="button"
          onClick={onClick}
          title={title}
          className="flex-1 min-w-0 flex items-center gap-2 h-8 px-2 rounded-md text-left text-[13px] text-fg hover:bg-surface-hi transition"
        >
          {content}
        </button>
      ) : (
        <div className="flex-1 min-w-0 flex items-center gap-2 h-8 px-2 text-[13px] text-fg" title={title}>
          {content}
        </div>
      )}
      {trailing}
    </div>
  );
}

function IconButton({ icon, label, onClick, disabled = false, large = false, className = '' }) {
  return (
    <button
      type="button"
      onClick={onClick}
      disabled={disabled}
      title={label}
      aria-label={label}
      className={clsx(
        large ? 'w-7 h-7' : 'w-6 h-6',
        'shrink-0 rounded-md flex items-center justify-center text-fg-mute transition',
        disabled ? 'opacity-40 cursor-not-allowed' : 'hover:bg-surface-hi hover:text-fg',
        className,
      )}
    >
      <VsIcon name={icon} size={large ? 17 : 13} />
    </button>
  );
}

function useWorkspaceDirectoryEntry(hash) {
  const getSnapshot = () => workspaceDirectoryEntry(hash);
  return useSyncExternalStore(subscribeWorkspaceDirectory, getSnapshot, getSnapshot);
}

// 目录行菜单:左键、右键打开同一份下拉菜单,从行下方展开、与行同宽(学 Claude 桌面版)。
// 走统一的 DesktopContextMenu,只给「打开文件夹」「复制文件路径」两项。产物行的右键
// 也用它(kind 传 'file',「打开文件夹」会在资源管理器里选中这个文件)。
function openFolderMenu(element, path, kind = 'directory') {
  if (!element || !path) return;
  const rect = element.getBoundingClientRect();
  openDesktopContextMenu({
    target: element,
    trigger: element,
    x: rect.left,
    y: rect.bottom + 4,
    placement: 'below',
    width: rect.width,
    showIcons: false,
    includeContextActions: false,
    leadingItems: [
      {
        id: DESKTOP_CONTEXT_ACTIONS.OPEN_IN_EXPLORER,
        group: 'summary-folder-open',
        target: { path, kind },
      },
      {
        id: DESKTOP_CONTEXT_ACTIONS.COPY_ABSOLUTE_PATH,
        label: '复制文件路径',
        group: 'summary-folder-copy',
        target: { absolutePath: path },
      },
    ],
  });
}

// 右键:行上标 data-desktop-context-menu-delegate,DesktopContextMenu 把右键广播过来,
// 这里认领后按左键同样的位置和宽度打开,不跟随光标。
function useFolderContextMenu(rootRef) {
  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail || {};
      if (detail.handled || detail.name !== SUMMARY_FOLDER_CONTEXT_MENU_DELEGATE) return;
      const element = detail.element;
      if (!element || !rootRef.current?.contains(element)) return;
      const path = element.getAttribute('data-summary-folder-path') || '';
      if (!path) return;
      detail.handled = true;
      openFolderMenu(element, path, element.getAttribute('data-summary-path-kind') || 'directory');
    };
    window.addEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
    return () => window.removeEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
  }, [rootRef]);
}

const FOLDER_TOOLTIP_DELAY_MS = 450;

// 悬停一会儿显示完整路径(行里只显示「目录名 · 上级路径」,窄的时候还会被截断)。
// 挂到 body 上,不被面板的滚动区裁掉;上方放不下时放到行下方。
function FolderRow({ path, primary }) {
  const rowRef = useRef(null);
  const timerRef = useRef(0);
  const [tip, setTip] = useState(null);
  const { name, parent } = folderLabel(path);
  const hide = () => {
    window.clearTimeout(timerRef.current);
    timerRef.current = 0;
    setTip(null);
  };
  const scheduleShow = () => {
    window.clearTimeout(timerRef.current);
    timerRef.current = window.setTimeout(() => {
      const rect = rowRef.current?.getBoundingClientRect();
      if (!rect) return;
      const above = rect.top > 72;
      setTip({ left: rect.left + 24, top: above ? rect.top : rect.bottom + 6, above });
    }, FOLDER_TOOLTIP_DELAY_MS);
  };
  useEffect(() => () => window.clearTimeout(timerRef.current), []);
  return (
    <div
      ref={rowRef}
      className="ace-summary-row flex items-center min-w-0"
      data-desktop-context-menu-delegate={SUMMARY_FOLDER_CONTEXT_MENU_DELEGATE}
      data-summary-folder-path={path}
      onMouseEnter={scheduleShow}
      onMouseLeave={hide}
      onPointerDown={hide}
    >
      <button
        type="button"
        aria-haspopup="menu"
        onClick={() => {
          hide();
          openFolderMenu(rowRef.current, path);
        }}
        className="flex-1 min-w-0 flex items-center gap-2 h-8 px-2 rounded-md text-left text-[13px] text-fg hover:bg-surface-hi transition"
      >
        <VsIcon name="folder" size={15} className="shrink-0 text-fg-mute" />
        <span className="flex-1 min-w-0 truncate">
          {name}
          {parent && <span className="text-fg-mute"> · {parent}</span>}
        </span>
      </button>
      {tip && createPortal(
        <div
          className="ace-summary-folder-tooltip"
          data-side={tip.above ? 'above' : 'below'}
          style={{ left: tip.left, top: tip.top }}
          role="tooltip"
        >
          <div className="ace-summary-folder-tooltip-title">{primary ? '工作目录' : '源文件夹'}</div>
          <div className="ace-summary-folder-tooltip-path">{path}</div>
        </div>,
        document.body,
      )}
    </div>
  );
}

const SUMMARY_ROW_BUTTON_CLASS = 'flex-1 min-w-0 flex items-center gap-2 h-8 px-2 rounded-md text-left text-[13px] text-fg hover:bg-surface-hi transition';

// 产物行:左键在右侧预览里打开;右键是和目录行一样的「打开文件夹 / 复制文件路径」。
function OutputRow({ row, onOpen }) {
  const content = (
    <>
      <FileTypeIcon path={row.name} size={15} className="shrink-0" />
      <span className="flex-1 min-w-0 truncate">
        {row.name}
        {row.hint && <span className="text-fg-mute"> · {row.hint}</span>}
      </span>
    </>
  );
  return (
    <div
      className="ace-summary-row flex items-center min-w-0"
      data-desktop-context-menu-delegate={SUMMARY_FOLDER_CONTEXT_MENU_DELEGATE}
      data-summary-folder-path={row.path}
      data-summary-path-kind="file"
    >
      {onOpen ? (
        <button type="button" onClick={() => onOpen(row.path)} title={row.title} className={SUMMARY_ROW_BUTTON_CLASS}>
          {content}
        </button>
      ) : (
        <div className="flex-1 min-w-0 flex items-center gap-2 h-8 px-2 text-[13px] text-fg" title={row.title}>
          {content}
        </div>
      )}
    </div>
  );
}

// 「产物」分组:先显示最近的几个,其余点「显示更多」;一个都没有时整段不画。
function OutputsSection({ outputs, onOpenFile }) {
  const [expanded, setExpanded] = useState(false);
  const model = sessionOutputRows(outputs, { expanded });
  if (model.rows.length === 0) return null;
  return (
    <>
      <div className="h-px bg-border mx-2 my-2" />
      <Section id="outputs" title="产物" collapsible>
        {model.rows.map((row) => (
          <OutputRow key={row.key} row={row} onOpen={onOpenFile} />
        ))}
        {model.expandable && (
          <button
            type="button"
            onClick={() => setExpanded((value) => !value)}
            aria-expanded={expanded}
            className="block w-full h-8 px-2 rounded-md text-left text-[13px] text-fg-mute hover:text-fg transition"
          >
            {expanded ? '显示更少' : '显示更多'}
          </button>
        )}
      </Section>
    </>
  );
}

export function SummaryPanel({
  owner,
  api,
  sessionId = '',
  cwd = '',
  workspaceName = '',
  workspaceHash = '',
  noWorkspace = false,
  sessionChangeSummary = null,
  busy = false,
  tasks = [],
  visible = true,
  onClose,
  onEditWorkspace,
  onOpenChanges,
  onOpenFile,
  onOpenTask,
  onOpenFinishedTasks,
  onStopTask,
}) {
  const rootRef = useRef(null);
  useFolderContextMenu(rootRef);
  const outputs = useSessionOutputs({
    api, sessionId, workspaceHash, busy, enabled: visible && !!sessionId,
  });
  const directory = useWorkspaceDirectoryEntry(noWorkspace ? '' : workspaceHash);
  const gitInfo = useGitInfo(api, cwd, owner, visible && !!cwd);
  const gitList = useGitChangeList({ api, owner, cwd, gitInfo, busy, enabled: visible && !!cwd });
  const workspace = summaryWorkspaceModel({
    name: workspaceName, cwd, hash: workspaceHash, noWorkspace, gitInfo, directory,
  });
  const changes = summaryChangeTotals({
    isRepo: !!gitInfo?.is_repo,
    gitList,
    sessionSummary: sessionChangeSummary,
  });
  const hasRunning = (tasks || []).some((task) => task?.status === 'running');
  const now = useNow(visible && hasRunning);
  const running = summaryRunningTasks(tasks, now);
  const finishedCount = summaryFinishedTaskCount(tasks);
  const closeButton = onClose && (
    <IconButton icon="close" label="收起摘要面板" onClick={onClose} />
  );

  return (
    <div ref={rootRef} className="ace-summary-panel h-full w-full min-w-0 flex flex-col gap-1 overflow-y-auto overflow-x-hidden px-3 py-3 bg-surface">
      <Section id="workspace" title="工作区" action={closeButton} collapsible>
        {workspace.name && (
          <Row
            icon="computer"
            iconNode={workspace.icon && (
              <WorkspaceIcon id={workspace.icon.id} color={workspace.icon.color} size={15} className="shrink-0" />
            )}
            trailing={workspace.editable && (
              <IconButton icon="settings" label="编辑项目" large
                onClick={() => onEditWorkspace?.(workspace.hash)} />
            )}
          >
            {workspace.name}
          </Row>
        )}
        {workspace.folders.map((folder, index) => (
          <FolderRow key={folder} path={folder} primary={index === 0} />
        ))}
        {workspace.branch && <Row icon="fork" title={workspace.branch}>{workspace.branch}</Row>}
      </Section>
      <OutputsSection outputs={outputs} onOpenFile={onOpenFile} />
      {changes && (
        <>
          <div className="h-px bg-border mx-2 my-2" />
          <Section title="变更">
            <Row icon="edit" onClick={onOpenChanges}>
              <span className="ace-change-add">+{formatChangeCount(changes.additions)}</span>{' '}
              <span className="ace-change-del">−{formatChangeCount(changes.deletions)}</span>
            </Row>
          </Section>
        </>
      )}
      {(running.length > 0 || finishedCount > 0) && (
        <>
          <div className="h-px bg-border mx-2 my-2" />
          <Section id="background" title="后台任务" collapsible>
            {running.map((task) => (
              <Row
                key={task.id}
                icon="terminal"
                iconNode={task.kind === 'agent' && (
                  <SubagentAvatar sessionId={task.id} expert={task.expert} status="running" />
                )}
                title={task.title}
                onClick={() => onOpenTask?.(task.id)}
                trailing={(
                  <>
                    <span className="shrink-0 text-[12px] tabular-nums text-fg-mute">{task.elapsed}</span>
                    <IconButton
                      icon="stopFilled"
                      label="停止"
                      disabled={task.stopping}
                      onClick={() => onStopTask?.(task.id)}
                      className="hover:text-danger"
                    />
                  </>
                )}
              >
                {task.title}
              </Row>
            ))}
            {finishedCount > 0 && (
              <Row icon="check" onClick={onOpenFinishedTasks}>
                {finishedCount} 已完成
              </Row>
            )}
          </Section>
        </>
      )}
    </div>
  );
}
