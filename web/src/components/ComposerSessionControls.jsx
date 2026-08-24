import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { loadTier, loadTierTextClass } from '../lib/modelLoad.js';
import { PERMISSION_MODES, normalizePermissionMode, permissionModeOption } from '../lib/permissionMode.js';
import { buildStatusBarModelMenu } from '../lib/sessionModel.js';
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

      const compactOrder = ['permission', 'expert', 'swarm-mode', 'model'];
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
    <svg
      width="16"
      height="16"
      viewBox="0 0 16 16"
      fill="none"
      aria-hidden="true"
      className={className}
    >
      <path
        d="M8 1.5 13 3.4v3.8c0 3.1-1.85 5.8-5 7.3-3.15-1.5-5-4.2-5-7.3V3.4L8 1.5Z"
        stroke="currentColor"
        strokeWidth="1.35"
        strokeLinejoin="round"
      />
      <path d="M8 4.5v4" stroke="currentColor" strokeWidth="1.35" strokeLinecap="round" />
      <circle cx="8" cy="11.1" r=".75" fill="currentColor" />
    </svg>
  );
}

function SignalBars() {
  return (
    <svg width="12" height="11" viewBox="0 0 12 11" aria-hidden="true" className="shrink-0">
      <rect x="0" y="8" width="2.2" height="3" rx="0.5" fill="currentColor" />
      <rect x="3.2" y="5.5" width="2.2" height="5.5" rx="0.5" fill="currentColor" />
      <rect x="6.4" y="3" width="2.2" height="8" rx="0.5" fill="currentColor" />
      <rect x="9.6" y="0.5" width="2.2" height="10.5" rx="0.5" fill="currentColor" />
    </svg>
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

export function ComposerSessionControls({
  addControl,
  contexts,
  actions,
  className = '',
  model = '—',
  modelOptions = [],
  selectedModelName = '',
  modelSwitching = false,
  modelRefreshing = false,
  onModelChange,
  onRefreshModels,
  onOpenModelSettings,
  modelLoad = null,
  tokenBudget = null,
  permissionMode = 'default',
  permissionSwitching = false,
  onPermissionModeChange,
  swarmMode = false,
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
  const compactControls = useAdaptiveComposerControls(
    rootRef,
    `${swarmMode}|${expertName}|${permissionMode}|${selectedModelName}|${model}`,
  );

  useEffect(() => {
    setLocalMode(normalizePermissionMode(permissionMode));
  }, [permissionMode]);

  useEffect(() => {
    if (!openMenu) return undefined;
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

        {swarmMode && (
          <div
            data-adaptive-composer-control="true"
            data-compact={compactControls.has('swarm-mode') ? 'true' : 'false'}
            data-composer-control="swarm-mode"
            role="status"
            aria-label="已开启蜂群模式"
            title="下一条普通消息将积极派遣子 Agent"
            className="ace-composer-adaptive-chip ace-composer-swarm-chip flex h-7 min-w-0 shrink items-center gap-1.5 rounded-md bg-accent-bg px-2 text-accent"
          >
            <SwarmModeIcon size={14} className="shrink-0" />
            <span className="ace-composer-adaptive-content text-[11px] font-medium">蜂群模式</span>
            <button
              type="button"
              onPointerDown={(event) => event.preventDefault()}
              onClick={onDisableSwarm}
              title="关闭蜂群模式"
              aria-label="关闭蜂群模式"
              className="ace-composer-adaptive-content flex h-4 w-4 shrink-0 items-center justify-center rounded text-accent opacity-70 hover:bg-accent-bg hover:opacity-100"
            >
              <VsIcon name="close" size={11} />
            </button>
          </div>
        )}

        {expertName && (
          <div
            data-adaptive-composer-control="true"
            data-compact={compactControls.has('expert') ? 'true' : 'false'}
            data-composer-control="expert"
            data-expert-id={expertId || undefined}
            data-expert-type={expertType === 'team' ? 'team' : 'agent'}
            title={`当前专家组件：${expertName}`}
            className="ace-composer-adaptive-chip ace-composer-expert-chip flex h-7 min-w-0 max-w-[210px] shrink items-center gap-1.5 rounded-md bg-accent-bg px-2 text-accent"
          >
            <span
              role="status"
              aria-label={`已派遣${expertType === 'team' ? '专家团' : '专家'}：${expertName}`}
              className="flex min-w-0 items-center gap-1.5"
            >
              <VsIcon name={expertType === 'team' ? 'extension' : 'brain'} size={14} className="shrink-0" />
              <span className="ace-composer-adaptive-content min-w-0 truncate text-[11px] font-medium">{expertName}</span>
              <span className="ace-composer-adaptive-content shrink-0 text-[9px] opacity-70">
                {expertType === 'team' ? '专家团' : '专家'}
              </span>
            </span>
            <button
              type="button"
              disabled={expertRemoving}
              onPointerDown={(event) => event.preventDefault()}
              onClick={onRemoveExpert}
              title={`解除${expertType === 'team' ? '专家团' : '专家'} ${expertName}`}
              aria-label={`解除${expertType === 'team' ? '专家团' : '专家'}：${expertName}`}
              className={clsx(
                'ace-composer-adaptive-content ml-auto flex h-4 w-4 shrink-0 items-center justify-center rounded text-accent opacity-70 hover:bg-accent-bg hover:opacity-100',
                expertRemoving && 'cursor-wait opacity-50',
              )}
            >
              <VsIcon name="close" size={11} />
            </button>
          </div>
        )}

        {pendingExpertName && (
          <div
            data-composer-control="expert-pending"
            data-expert-type={pendingExpertType === 'team' ? 'team' : 'agent'}
            role="status"
            aria-live="polite"
            aria-label={`下一轮派遣${pendingExpertType === 'team' ? '专家团' : '专家'}：${pendingExpertName}`}
            title={`当前轮保持原专家；下一轮派遣${pendingExpertName}`}
            className="flex h-7 min-w-0 max-w-[220px] items-center gap-1.5 rounded-md bg-surface-hi px-2 text-warn"
          >
            <VsIcon name="running" size={13} mono={false} className="shrink-0" />
            <span className="shrink-0 text-[9px]">下一轮</span>
            <span className="min-w-0 truncate text-[11px] font-medium">{pendingExpertName}</span>
            <button
              type="button"
              disabled={expertRemoving}
              onPointerDown={(event) => event.preventDefault()}
              onClick={onRemoveExpert}
              title={`取消派遣 ${pendingExpertName}`}
              aria-label={`取消派遣：${pendingExpertName}`}
              className={clsx(
                'ml-auto flex h-4 w-4 shrink-0 items-center justify-center rounded text-warn opacity-70 hover:bg-surface-alt hover:opacity-100',
                expertRemoving && 'cursor-wait opacity-50',
              )}
            >
              <VsIcon name="close" size={11} />
            </button>
          </div>
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

        <div data-composer-control="submit" className="flex shrink-0 items-center gap-1">
          {actions}
        </div>
      </div>
    </div>
  );
}
