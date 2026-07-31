// 新会话的分支 + worktree pill(openspec add-webui-git-session-pill)。
//
// 两个挂载点(variant):
//   hero — 首页新对话视图,「项目」pill 右侧(始终显示)
//   bar  — 聊天布局 InputBar 下方,仅「未开始的新会话」显示;会话一旦有消息
//          (或已运行在 worktree)即整体隐藏 —— 切到已有会话不再显示分支/worktree。
//
// 交互语义(上游对话拍板):
//   - 分支选择只辅助 worktree 创建;未勾 worktree 时禁用,不允许从这里
//     切换主仓分支。
//   - 勾了 worktree 后才能选择「worktree 基线」;真正创建发生在首条消息
//     发送时(父组件经 onIntentChange 拿到意图)。
// 纯状态逻辑在 lib/gitSessionPill.js(Node 单测),这里只做 DOM 映射。

import { useCallback, useEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { gitInfoCache } from '../lib/gitInfoCache.js';
import { buildPillModel } from '../lib/gitSessionPill.js';
import { VsIcon } from './Icon.jsx';

export function GitSessionPill({
  cwd,
  variant = 'bar',
  sessionStarted = false,
  worktreeSession = null,
  busy = false,
  onIntentChange = null,
}) {
  const [gitInfo, setGitInfo] = useState(null);
  const [open, setOpen] = useState(false);
  const [worktreeChecked, setWorktreeChecked] = useState(false);
  const [selectedBase, setSelectedBase] = useState('');
  const cwdRef = useRef(cwd);
  cwdRef.current = cwd;

  // 这个 pill 在 ChatView 上的 key 含 sid,每次切会话都整组件重挂。
  // /api/git/info 在 daemon 侧要 spawn 5~7 个 git 子进程,所以:
  //   1) 读取一律走共享缓存(30s TTL + 在途去重),与 Sidebar hover 卡片
  //      共用同一份 —— 同 workspace 的会话 cwd 相同,命中率极高;
  //   2) bar 变体在「会话已开始」时整体 return null,这种情况下**根本不发
  //      请求**。切到任何一个已有消息的会话原本都会白白打一次 git.exe ×6,
  //      这是用户在 Fiddler 里看到 /api/git/info 高频的主因。
  const inWorktree = !!(worktreeSession
    && (worktreeSession.name || worktreeSession.branch));
  const pillCouldRender = variant === 'hero' || !(sessionStarted || inWorktree);

  const refreshInfo = useCallback(({ force = false } = {}) => {
    const target = cwdRef.current;
    if (!target) { setGitInfo(null); return; }
    if (force) gitInfoCache.invalidate(target);
    gitInfoCache.get(target)
      .then((info) => { if (cwdRef.current === target) setGitInfo(info); })
      .catch(() => { if (cwdRef.current === target) setGitInfo(null); });
  }, []);

  // cwd 变化(切 workspace)重拉;非仓库时 pill 整体不渲染(零占位)。
  useEffect(() => {
    setGitInfo(null);
    setWorktreeChecked(false);
    setSelectedBase('');
    if (!pillCouldRender) return;
    refreshInfo();
  }, [cwd, pillCouldRender, refreshInfo]);

  // 把待生效意图同步给父组件(首条消息发送时读取)。
  useEffect(() => {
    onIntentChange?.({
      worktreeChecked,
      selectedBase: selectedBase || gitInfo?.branch || '',
    });
  }, [worktreeChecked, selectedBase, gitInfo?.branch, onIntentChange]);

  const model = buildPillModel({
    gitInfo, worktreeChecked, sessionStarted, worktreeSession, busy,
  });
  if (!model.visible) return null;
  // bar 变体只在"未开始的新会话"露出分支/worktree 选择;会话一旦开始
  // (有消息 / 已在 worktree)即整体隐藏。hero(新对话界面)始终显示。
  if (variant !== 'hero' && model.started) return null;

  const pickBranch = (branch) => {
    if (!model.branchInteractive) return;
    setOpen(false);
    // worktree 基线选择:纯前端状态,不动主仓。
    setSelectedBase(branch);
  };

  const displayBranch = worktreeChecked && selectedBase ? selectedBase : model.branch;

  return (
    <div className={clsx(
      'relative flex items-center',
      // bar:与 InputBar dock 同色(bg-surface,消色差),-mt-1.5 向上贴近输入框 6px。
      // hero(新对话界面):沿用首页布局,无背景/偏移。
      variant === 'hero' ? '' : 'px-3 pb-1 -mt-1.5 bg-surface',
    )}>
      <div
        className={clsx(
          'ace-git-pill group inline-flex items-center gap-0 rounded-lg border border-border bg-surface text-[12px] overflow-hidden',
          !model.interactive && 'opacity-80',
        )}
        data-variant={variant}
      >
        <button
          type="button"
          className={clsx(
            'inline-flex items-center gap-1.5 px-2.5 py-1 transition-colors',
            model.branchInteractive
              ? 'hover:bg-surface-hi cursor-pointer'
              : 'text-fg-mute cursor-not-allowed opacity-60',
          )}
          disabled={!model.branchInteractive}
          onClick={() => {
            if (!model.branchInteractive) return;
            // 打开下拉时重拉 info:外部新建/切换的分支即时可见。这是显式的
            // 用户动作,绕过缓存强制刷新。
            if (!open) refreshInfo({ force: true });
            setOpen(!open);
          }}
          title={model.started
            ? '会话进行中,分支不可在此选择'
            : !model.interactive
              ? '会话运行中,暂时不能选择分支'
              : !model.worktreeChecked
                ? '勾选 worktree 后选择基线分支'
                : 'worktree 基线分支(发送首条消息时创建 worktree)'}
        >
          <VsIcon name="fork" size={13} className="opacity-70" />
          <span className="font-medium truncate max-w-[160px]">{displayBranch}</span>
          {model.branchInteractive && (
            <VsIcon name="expandDown" size={12} className="opacity-50 group-hover:opacity-100 transition-opacity" />
          )}
        </button>
        <div className="w-px self-stretch bg-border" aria-hidden="true" />
        <label
          className={clsx(
            'inline-flex items-center gap-1.5 px-2.5 py-1 select-none transition-colors',
            model.interactive ? 'hover:bg-surface-hi cursor-pointer' : 'cursor-default',
          )}
          title={model.worktreeBadge
            ? `会话运行在 worktree:${model.worktreeBadge}`
            : '在独立的 git worktree 中运行本会话(发送首条消息时创建)'}
        >
          <input
            type="checkbox"
            className="ace-git-pill-checkbox"
            checked={model.worktreeChecked}
            disabled={!model.interactive}
            onChange={(e) => {
              const checked = e.target.checked;
              setWorktreeChecked(checked);
              if (!checked) {
                setOpen(false);
                setSelectedBase('');
              }
            }}
          />
          <span className={clsx('text-fg-mute', model.worktreeChecked && 'text-fg')}>
            {model.worktreeBadge ? `worktree:${model.worktreeBadge}` : 'worktree'}
          </span>
        </label>
      </div>

      {open && model.branchInteractive && (
        <>
          <div className="fixed inset-0 z-40" onClick={() => setOpen(false)} />
          <div className="absolute bottom-full left-0 mb-1.5 min-w-[200px] max-w-[300px] max-h-[40vh] overflow-y-auto bg-surface border border-border ace-shadow rounded-xl z-50 py-1.5 ace-scrollbar"
               data-placement={variant === 'hero' ? 'below' : 'above'}
               style={variant === 'hero' ? { bottom: 'auto', top: '100%', marginBottom: 0, marginTop: 6 } : undefined}>
            <div className="px-3 pb-1 mb-1 text-[11px] font-semibold text-fg-mute border-b border-border/50 uppercase tracking-wider">
              worktree 基线分支
            </div>
            {model.branches.map((b) => (
              <button
                key={b}
                type="button"
                className={clsx(
                  'w-full text-left px-3 py-1.5 text-[13px] flex items-center gap-2 transition-colors',
                  b === displayBranch ? 'bg-accent/10 text-accent font-medium' : 'text-fg hover:bg-surface-hi',
                )}
                onClick={() => pickBranch(b)}
              >
                <VsIcon name="fork" size={12} className="opacity-60" />
                <span className="truncate">{b}</span>
                {b === model.branch && (
                  <span className="ml-auto text-[10px] text-fg-mute">当前</span>
                )}
              </button>
            ))}
          </div>
        </>
      )}

    </div>
  );
}
