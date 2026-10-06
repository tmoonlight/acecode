import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { loadTier, loadTierTextClass } from '../lib/modelLoad.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { buildStatusBarModelMenu } from '../lib/sessionModel.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { RefreshIcon, VsIcon } from './Icon.jsx';
import { SwarmModeIcon } from './SwarmModeIcon.jsx';
import { TokenBudgetRing } from './TokenBudgetRing.jsx';
import { ProviderIcon } from './model-settings/ProviderIcon.jsx';

function permissionTextClass(color) {
  if (color === 'ok') return 'text-ok';
  if (color === 'warn') return 'text-warn';
  return 'text-danger';
}

function useAdaptiveComposerControls(rootRef, measureKey) {
  const [compactControls, setCompactControls] = useState(() => new Set());

  useLayoutEffect(() => {
    const root = rootRef.current;
    if (!root) return undefined;
    let frame = 0;
    const update = () => {
      const controls = [...root.querySelectorAll('[data-adaptive-composer-control="true"]')];
      if (controls.length === 0) return;
      root.setAttribute('data-ultra-compact', 'false');

      controls.forEach((element) => {
        element.setAttribute('data-compact', 'false');
        element.style.width = 'max-content';
        element.style.flex = '0 0 auto';
      });
      root.getBoundingClientRect();

      const desiredWidths = controls.map((element) => {
        const cap = Number.parseFloat(getComputedStyle(element).maxWidth);
        const natural = Math.ceil(Math.max(element.getBoundingClientRect().width, element.scrollWidth));
        return Number.isFinite(cap) ? Math.min(natural, cap) : natural;
      });

      controls.forEach((element, index) => {
        element.style.width = '';
        element.style.flex = `0 0 ${desiredWidths[index]}px`;
      });
      root.getBoundingClientRect();

      const isCrowded = () => {
        const rootRect = root.getBoundingClientRect();
        const right = root.querySelector('.ace-composer-session-right');
        const leftControls = [...root.querySelectorAll('.ace-composer-session-left > [data-composer-control]')]
          .filter((element) => element.getBoundingClientRect().width > 0);
        const leftEdge = leftControls.length > 0
          ? Math.max(...leftControls.map((element) => element.getBoundingClientRect().right))
          : rootRect.left;
        const rightRect = right?.getBoundingClientRect();
        return root.scrollWidth > root.clientWidth + 1
          || (rightRect && rightRect.right > rootRect.right + 1)
          || (rightRect && leftEdge > rightRect.left - 1);
      };

      const compactOrder = ['permission', 'expert-pending', 'expert', 'swarm-mode', 'goal', 'model'];
      const nextCompact = new Set();
      for (const controlName of compactOrder) {
        if (!isCrowded()) break;
        const element = controls.find((item) => item.dataset.composerControl === controlName);
        if (!element) continue;
        nextCompact.add(controlName);
        element.setAttribute('data-compact', 'true');
        element.style.flex = '0 0 28px';
        root.getBoundingClientRect();
      }
      controls.forEach((element) => {
        if (nextCompact.has(element.dataset.composerControl)) return;
        const contentClipped = [...element.querySelectorAll('.ace-composer-adaptive-content')]
          .some((content) => getComputedStyle(content).display !== 'none'
            && content.scrollWidth > content.clientWidth + 1);
        if (!contentClipped) return;
        nextCompact.add(element.dataset.composerControl);
        element.setAttribute('data-compact', 'true');
        element.style.flex = '0 0 28px';
        root.getBoundingClientRect();
      });
      if (isCrowded()) {
        root.setAttribute('data-ultra-compact', 'true');
        root.getBoundingClientRect();
      }
      controls.forEach((element) => {
        element.setAttribute(
          'data-compact',
          nextCompact.has(element.dataset.composerControl) ? 'true' : 'false',
        );
      });
      setCompactControls((current) => {
        if (current.size === nextCompact.size
          && [...current].every((item) => nextCompact.has(item))) return current;
        return nextCompact;
      });
    };
    const schedule = () => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(update);
    };
    update();
    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', schedule);
      return () => {
        cancelAnimationFrame(frame);
        window.removeEventListener('resize', schedule);
      };
    }
    const observer = new ResizeObserver(schedule);
    observer.observe(root);
    window.addEventListener('resize', schedule);
    return () => {
      cancelAnimationFrame(frame);
      observer.disconnect();
      window.removeEventListener('resize', schedule);
    };
  }, [measureKey]);

  return compactControls;
}

function PermissionShieldIcon({ className = '' }) {
  return (
    <VsIcon name="ShieldWarning" size={16} className={className} />
  );
}

function SignalBars() {
  return (
    <VsIcon name="Signal" size={12} className="shrink-0" style={{ height: 11 }} />
  );
}

function ModelLoadIndicator({ load }) {
  if (!load) return null;
  const tier = loadTier(load.usageRate);
  if (!tier) return null;
  const percent = Math.round(load.usageRate);
  const effectiveWindow = load.effectiveContextWindow
    ? `，有效上下文 ${Math.round(load.effectiveContextWindow / 1000)}k`
    : '';
  return (
    <span
      data-composer-control="model-load"
      className={clsx('ace-composer-model-load', loadTierTextClass(tier))}
      title={`模型池负载 ${percent}%${effectiveWindow}`}
    >
      <SignalBars />
      <span className="tabular-nums">{percent}%</span>
    </span>
  );
}

function ComposerSelectionTag({ icon, compact, status, pending = false, children, ...props }) {
  return (
    <button
      {...props}
      type="button"
      data-adaptive-composer-control="true"
      data-compact={compact ? 'true' : 'false'}
      onPointerDown={(event) => event.preventDefault()}
      className={clsx('ace-composer-adaptive-chip', pending && 'ace-composer-pending-chip')}
    >
      <span className="ace-composer-chip-icon" aria-hidden="true">
        <span className="ace-composer-chip-symbol">{icon}</span>
        <VsIcon name="close" size={16} className="ace-composer-chip-remove" />
      </span>
      <span className="ace-composer-adaptive-content min-w-0 truncate">{children}</span>
      {status && <span role="status" className="sr-only">{status}</span>}
    </button>
  );
}

export function ComposerSessionControls({
  addControl,
  goalMode = false,
  goalDisabled = false,
  onDisableGoal,
  contexts,
  actions,
  className = '',
  model = '—',
  modelOptions = [],
  selectedModelName = '',
  modelSwitching = false,
  modelRefreshing = false,
  reasoningOptions = null,
  reasoningDisabled = false,
  onReasoningChange,
  onCaptureComposerSelection,
  onModelChange,
  onRefreshModels,
  onOpenModelSettings,
  modelLoad = null,
  tokenBudget = null,
  permissionMode = 'default',
  permissionSwitching = false,
  onPermissionModeChange,
  swarmMode = 'off',
  onDisableSwarm,
  expertId = '',
  expertName = '',
  expertType = 'agent',
  expertRemoving = false,
  onRemoveExpert,
  pendingExpertName = '',
  pendingExpertType = 'agent',
}) {
  const [localMode, setLocalMode] = useState(normalizePermissionMode(permissionMode));
  const [openMenu, setOpenMenu] = useState('');
  const rootRef = useRef(null);
  const reasoningAnchorRef = useRef(null);
  const reasoningCaretRestoreRef = useRef(null);
  const closeReasoningMenu = () => {
    setOpenMenu('');
    const restore = reasoningCaretRestoreRef.current;
    reasoningCaretRestoreRef.current = null;
    if (restore) window.requestAnimationFrame(restore);
  };
  // 蜂群模式芯片:星型 / 网状各一套文案(add-mesh-swarm-mode),关闭时不显示。
  const swarmTag = swarmMode === 'mesh'
    ? {
        label: '蜂群模式（网状）',
        status: '已开启蜂群模式（网状）',
        title: 'Agent 之间可以互相派任务、发消息，组成协作网络',
      }
    : swarmMode === 'star'
      ? {
          label: '蜂群模式（星型）',
          status: '已开启蜂群模式（星型）',
          title: '主 Agent 积极派遣子 Agent 并汇总结果',
        }
      : null;
  const compactControls = useAdaptiveComposerControls(
    rootRef,
    `${goalMode}|${swarmMode}|${expertName}|${pendingExpertName}|${permissionMode}|${selectedModelName}|${model}|${reasoningOptions?.label || ''}`,
  );

  useEffect(() => {
    setLocalMode(normalizePermissionMode(permissionMode));
  }, [permissionMode]);

  useEffect(() => {
    if (!openMenu || openMenu === 'reasoning') return undefined;
    const onPointerDown = (event) => {
      if (!rootRef.current?.contains(event.target)) setOpenMenu('');
    };
    const onKeyDown = (event) => {
      if (event.key === 'Escape') setOpenMenu('');
    };
    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [openMenu]);

  useEffect(() => {
    if (openMenu === 'reasoning' && (!reasoningOptions || reasoningDisabled)) closeReasoningMenu();
  }, [reasoningOptions, reasoningDisabled, openMenu]);

  const mode = normalizePermissionMode(onPermissionModeChange ? permissionMode : localMode);
  const permission = permissionModeOption(mode);
  const modelMenu = useMemo(() => buildStatusBarModelMenu({
    modelOptions,
    selectedModelName,
    fallbackLabel: model,
  }), [model, modelOptions, selectedModelName]);
  const modelDeleted = modelMenu.displayDeleted || String(model || '').includes('(deleted)');
  const compactModelLabel = modelDeleted && selectedModelName
    ? `${selectedModelName} (deleted)`
    : (selectedModelName || modelMenu.displayLabel);
  const canOpenModelMenu = modelOptions.length > 0 || !!onRefreshModels;
  const modelBusy = modelSwitching || modelRefreshing;

  const selectPermission = (nextMode) => {
    const normalized = normalizePermissionMode(nextMode);
    if (onPermissionModeChange) onPermissionModeChange(normalized);
    else setLocalMode(normalized);
    setOpenMenu('');
  };

  const selectModel = (name) => {
    const nextName = String(name || '');
    if (!nextName || nextName === selectedModelName || modelBusy) return;
    setOpenMenu('');
    onModelChange?.(nextName);
  };

  const openModelSettings = () => {
    setOpenMenu('');
    onOpenModelSettings?.();
  };

  return (
    <div
      ref={rootRef}
      data-composer-session-controls="true"
      className={clsx('ace-composer-session-footer', className)}
    >
      <div className="ace-composer-session-left">
        <div
          data-composer-control="add-context"
          className="shrink-0"
          onPointerDown={() => setOpenMenu('')}
        >
          {addControl}
        </div>

        {goalMode && (
          <ComposerSelectionTag
            data-composer-control="goal"
            compact={compactControls.has('goal')}
            icon={<VsIcon name="Goal" size={16} />}
            disabled={goalDisabled}
            onClick={onDisableGoal}
            title="取消目标"
            aria-label="取消目标"
          >
            目标
          </ComposerSelectionTag>
        )}

        {swarmTag && (
          <ComposerSelectionTag
            key={swarmMode}
            data-composer-control="swarm-mode"
            data-swarm-mode={swarmMode}
            compact={compactControls.has('swarm-mode')}
            icon={<SwarmModeIcon size={16} />}
            onClick={onDisableSwarm}
            status={swarmTag.status}
            aria-label="关闭蜂群模式"
            title={swarmTag.title}
          >
            {swarmTag.label}
          </ComposerSelectionTag>
        )}

        {expertName && (
          <ComposerSelectionTag
            key={`${expertType}:${expertId || expertName}`}
            data-composer-control="expert"
            compact={compactControls.has('expert')}
            icon={<VsIcon name="expert" size={16} />}
            data-expert-id={expertId || undefined}
            data-expert-type={expertType === 'team' ? 'team' : 'agent'}
            disabled={expertRemoving}
            onClick={onRemoveExpert}
            status={`已派遣${expertType === 'team' ? '专家团' : '专家'}：${expertName}`}
            aria-label={`解除${expertType === 'team' ? '专家团' : '专家'}：${expertName}`}
            title={`当前专家组件：${expertName}`}
          >
            {expertName}
          </ComposerSelectionTag>
        )}

        {pendingExpertName && (
          <ComposerSelectionTag
            key={`${pendingExpertType}:${pendingExpertName}`}
            data-composer-control="expert-pending"
            compact={compactControls.has('expert-pending')}
            icon={<VsIcon name="running" size={16} mono={false} />}
            data-expert-type={pendingExpertType === 'team' ? 'team' : 'agent'}
            pending
            disabled={expertRemoving}
            onClick={onRemoveExpert}
            status={`下一轮派遣${pendingExpertType === 'team' ? '专家团' : '专家'}：${pendingExpertName}`}
            aria-label={`取消派遣：${pendingExpertName}`}
            title={`当前轮保持原专家；下一轮派遣${pendingExpertName}`}
          >
            <span className="mr-1">下一轮</span>
            {pendingExpertName}
          </ComposerSelectionTag>
        )}

        <div
          data-adaptive-composer-control="true"
          data-compact={compactControls.has('permission') ? 'true' : 'false'}
          data-composer-control="permission"
          className="ace-composer-permission-control relative min-w-0"
        >
          <button
            type="button"
            disabled={permissionSwitching}
            onClick={() => setOpenMenu((current) => (current === 'permission' ? '' : 'permission'))}
            title={permission.hint}
            aria-haspopup="menu"
            aria-expanded={openMenu === 'permission'}
            className={clsx(
              'ace-composer-control-button ace-composer-permission-button',
              permissionTextClass(permission.color),
              permissionSwitching && 'cursor-wait opacity-60',
            )}
          >
            <PermissionShieldIcon className="shrink-0" />
            <span className="ace-composer-adaptive-content ace-composer-permission-label">{permission.label}</span>
            <VsIcon name="glyphDown" size={10} className="ace-composer-adaptive-content shrink-0 opacity-75" />
          </button>

          {openMenu === 'permission' && (
            <div
              role="menu"
              data-ace-native-overlay="overlap"
              aria-label="选择权限模式"
              className="ace-composer-popup ace-composer-permission-menu"
            >
              {PERMISSION_MODES.map((item) => {
                const active = item.id === mode;
                return (
                  <button
                    key={item.id}
                    type="button"
                    role="menuitemradio"
                    aria-checked={active}
                    disabled={permissionSwitching}
                    onClick={() => selectPermission(item.id)}
                    className={clsx(
                      'ace-composer-permission-option',
                      active ? 'bg-accent-bg' : 'hover:bg-surface-hi',
                    )}
                  >
                    <span className="flex items-center gap-1.5">
                      <PermissionShieldIcon
                        className={clsx('shrink-0', permissionTextClass(item.color))}
                      />
                      <span className={clsx(
                        'text-[12px]',
                        active ? 'font-semibold text-accent' : 'text-fg',
                      )}
                      >
                        {item.label}
                      </span>
                    </span>
                    <span className="pl-[22px] text-[10px] text-fg-mute">{item.hint}</span>
                  </button>
                );
              })}
            </div>
          )}
        </div>

        <div
          data-composer-control="selected-contexts"
          className="ace-composer-context-strip"
          tabIndex={contexts ? 0 : undefined}
          aria-label={contexts ? '已选上下文' : undefined}
        >
          {contexts}
        </div>
      </div>

      <div className="ace-composer-session-right">
        <ModelLoadIndicator load={modelLoad} />
        {tokenBudget && (
          <span data-composer-control="token-budget" className="inline-flex shrink-0">
            <TokenBudgetRing budget={tokenBudget} className="ace-composer-token-budget" />
          </span>
        )}

        <div
          data-adaptive-composer-control="true"
          data-compact={compactControls.has('model') ? 'true' : 'false'}
          data-composer-control="model"
          className="ace-composer-model-control relative min-w-0"
        >
          <button
            type="button"
            disabled={modelSwitching || !canOpenModelMenu}
            onClick={() => {
              if (!canOpenModelMenu || modelSwitching) return;
              setOpenMenu((current) => (current === 'model' ? '' : 'model'));
            }}
            title={modelMenu.displayLabel}
            aria-haspopup="listbox"
            aria-expanded={openMenu === 'model'}
            className={clsx(
              'ace-composer-control-button ace-composer-model-button',
              modelDeleted ? 'text-danger' : 'text-fg',
              (modelSwitching || !canOpenModelMenu) && 'cursor-wait opacity-60',
            )}
          >
            <ProviderIcon
              provider={modelMenu.selectedOption}
              size="sm"
              className="ace-composer-model-glyph"
            />
            <span className="ace-composer-adaptive-content ace-composer-model-label">{compactModelLabel}</span>
            <VsIcon name="glyphDown" size={10} className="ace-composer-adaptive-content shrink-0 opacity-75" />
          </button>

          {openMenu === 'model' && (
            <div
              className="ace-composer-popup ace-composer-model-menu"
              data-ace-native-overlay="overlap"
            >
              <div className="ace-composer-model-menu-header">
                <button
                  type="button"
                  onClick={openModelSettings}
                  title="打开模型设置"
                  className="ace-composer-model-settings"
                >
                  <VsIcon name="settings" size={14} className="shrink-0" />
                  <span>模型设置</span>
                </button>
                {onRefreshModels && (
                  <button
                    type="button"
                    disabled={modelRefreshing}
                    onClick={(event) => {
                      event.stopPropagation();
                      onRefreshModels();
                    }}
                    title="刷新模型列表"
                    aria-label="刷新模型列表"
                    className="ace-composer-model-refresh"
                  >
                    <RefreshIcon size={16} className={modelRefreshing ? 'animate-spin' : ''} />
                  </button>
                )}
              </div>
              <div role="listbox" aria-label="选择模型" className="ace-composer-model-options">
                {modelMenu.items.length > 0 ? modelMenu.items.map((item) => (
                  <button
                    key={item.name}
                    type="button"
                    role="option"
                    aria-selected={item.active}
                    disabled={modelBusy}
                    onClick={() => selectModel(item.name)}
                    className={clsx(
                      'ace-composer-model-option',
                      item.deleted
                        ? (item.active ? 'bg-danger-bg text-danger' : 'text-danger hover:bg-danger-bg')
                        : (item.active ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi'),
                      modelBusy && 'cursor-wait opacity-60',
                    )}
                    title={item.label}
                  >
                    <ProviderIcon provider={item} size="sm" />
                    <span className="min-w-0 flex-1 truncate text-[12px]">{item.label}</span>
                    <span className="w-3 shrink-0 text-center">
                      {item.active && <VsIcon name="ok" size={11} mono={false} />}
                    </span>
                  </button>
                )) : (
                  <div className="px-3 py-4 text-center text-[11px] text-fg-mute">
                    暂无可用模型
                  </div>
                )}
              </div>
            </div>
          )}
        </div>

        {reasoningOptions && (
          <div data-composer-control="reasoning" className="ace-composer-reasoning-control">
            <button
              ref={reasoningAnchorRef}
              type="button"
              disabled={reasoningDisabled}
              title="思考深度"
              aria-label={`思考深度：${reasoningOptions.label}`}
              aria-haspopup="menu"
              aria-expanded={openMenu === 'reasoning'}
              className="ace-composer-control-button ace-composer-reasoning-button text-fg disabled:cursor-not-allowed disabled:opacity-50"
              onPointerDown={() => {
                if (openMenu !== 'reasoning') reasoningCaretRestoreRef.current = onCaptureComposerSelection?.();
              }}
              onClick={() => {
                if (reasoningDisabled) return;
                if (openMenu === 'reasoning') closeReasoningMenu();
                else setOpenMenu('reasoning');
              }}
            >
              <span>{reasoningOptions.label}</span>
              <VsIcon name="glyphDown" size={10} className="shrink-0 opacity-75" />
            </button>
            {openMenu === 'reasoning' && (
              <AnchoredMenu
                anchorRef={reasoningAnchorRef}
                onClose={closeReasoningMenu}
                width={168}
                preferredPlacement="above"
                role="menu"
                aria-label="思考深度"
                className="ace-composer-reasoning-menu"
              >
                <div className="px-2 py-1.5 text-[11px] font-medium text-fg-mute">思考深度</div>
                {reasoningOptions.items.map((item) => (
                  <button
                    key={item.effort || 'default'}
                    type="button"
                    role="menuitemradio"
                    aria-checked={item.effort === reasoningOptions.selectedEffort}
                    disabled={reasoningDisabled}
                    className={clsx(
                      'flex w-full items-center justify-between gap-3 rounded-md px-2 py-1.5 text-left text-[12px] focus:outline-none focus-visible:bg-surface-hi',
                      item.effort === reasoningOptions.selectedEffort ? 'bg-accent-bg text-accent' : 'text-fg hover:bg-surface-hi',
                    )}
                    onClick={() => {
                      if (reasoningDisabled) return;
                      closeReasoningMenu();
                      if (item.effort !== reasoningOptions.selectedEffort) onReasoningChange?.(item.effort);
                    }}
                  >
                    <span>{item.label}</span>
                    {item.effort === reasoningOptions.selectedEffort && <VsIcon name="ok" size={11} mono={false} />}
                  </button>
                ))}
              </AnchoredMenu>
            )}
          </div>
        )}

        <div data-composer-control="submit" className="flex shrink-0 items-center gap-1">
          {actions}
        </div>
      </div>
    </div>
  );
}
