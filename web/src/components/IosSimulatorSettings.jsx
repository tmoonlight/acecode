import { useCallback, useEffect, useId, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { clsx } from '../lib/format.js';
import { normalizeSimulatorDevices, simulatorDeviceSubtitle } from '../lib/iosSimulator.js';
import {
  IOS_SIMULATOR_AUTO_XCODE,
  IOS_SIMULATOR_CUSTOM_XCODE,
  IOS_SIMULATOR_FPS_OPTIONS,
  IOS_SIMULATOR_SCALE_OPTIONS,
  inspectResultView,
  iosSimulatorErrorMessage,
  iosSimulatorNotices,
  iosSimulatorQualityOptions,
  iosSimulatorScaleLabel,
  isSelectedXcodeInstall,
  nearestOption,
  pickXcodePath,
  selectedXcodeChoice,
  xcodeInstallTitle,
  xcodeInstalls,
  xcodeSourceLabels,
} from '../lib/iosSimulatorSettings.js';
import { IosSimulatorIcon } from './IosSimulatorPanel.jsx';
import { Toggle } from './Modal.jsx';
import { RefreshIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const SELECT_CLASS = 'h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const LINK_BUTTON = 'px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50';
const NOTICE_TONE = { danger: 'text-danger', warn: 'text-warn', info: 'text-fg-mute' };

function XcodeOption({ name, value, checked, disabled, title, detail, tags = [], badge = '', onSelect }) {
  return (
    <label className={clsx('relative flex items-start gap-2.5 px-3 py-2 rounded-md border mb-1.5 cursor-pointer transition',
      checked ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:bg-surface-hi',
      disabled && 'opacity-60 cursor-default')}>
      <input type="radio" name={name} value={value} checked={checked} disabled={disabled}
        className="mt-0.5 accent-[var(--ace-accent)]" onChange={() => onSelect?.(value)} />
      <span className="flex-1 min-w-0">
        <span className="flex flex-wrap items-center gap-1.5">
          <span className="text-[13px] text-fg">{title}</span>
          {tags.map((tag) => (
            <span key={tag} className="text-[10px] px-1.5 py-px rounded border border-border text-fg-mute">{tag}</span>
          ))}
          {badge && <span className="text-[10px] px-1.5 py-px rounded border border-ok-border bg-ok-bg text-ok">{badge}</span>}
        </span>
        {detail && <span className="block text-[11px] text-fg-mute mt-0.5 break-all">{detail}</span>}
      </span>
    </label>
  );
}

export function IosSimulatorSettings() {
  const configId = useId();
  const radioName = useId();
  const [status, setStatus] = useState(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [rescanning, setRescanning] = useState(false);
  const [error, setError] = useState(null);
  const [expanded, setExpanded] = useState(false);
  const [customPath, setCustomPath] = useState('');
  const [inspecting, setInspecting] = useState(false);
  const [inspectResult, setInspectResult] = useState(null);
  const [devices, setDevices] = useState([]);
  const [devicesError, setDevicesError] = useState('');
  const [devicesLoading, setDevicesLoading] = useState(false);
  const [deviceBusy, setDeviceBusy] = useState('');
  const mountedRef = useRef(true);
  const autoExpandedRef = useRef(false);
  const customPathTouchedRef = useRef(false);

  useEffect(() => {
    mountedRef.current = true;
    return () => { mountedRef.current = false; };
  }, []);

  const applyStatus = useCallback((next) => {
    if (!mountedRef.current || !next) return;
    setStatus(next);
    // 自定义路径输入框跟随已保存的配置,用户正在改时不覆盖。
    if (!customPathTouchedRef.current) {
      setCustomPath(selectedXcodeChoice(next) === IOS_SIMULATOR_CUSTOM_XCODE ? String(next.config?.xcode_path || '') : '');
    }
  }, []);

  const load = useCallback(async ({ refresh = false } = {}) => {
    if (refresh) setRescanning(true); else setLoading(true);
    try {
      const next = await api.getIosSimulatorStatus({ refresh });
      applyStatus(next);
      if (mountedRef.current) setError(null);
      // 首次加载发现 Xcode 有问题时直接展开,方便从模拟器面板的「打开设置」跳过来处理。
      if (!autoExpandedRef.current && next?.supported !== false) {
        autoExpandedRef.current = true;
        if (iosSimulatorNotices(next).some((notice) => notice.kind !== 'info')) setExpanded(true);
      }
    } catch (loadError) {
      if (mountedRef.current) setError({ action: 'load', text: iosSimulatorErrorMessage(loadError, 'load') });
    } finally {
      if (mountedRef.current) {
        if (refresh) setRescanning(false); else setLoading(false);
      }
    }
  }, [applyStatus]);

  useEffect(() => { void load(); }, [load]);

  // inline=true 时失败原因交给调用方就地显示(自定义路径校验失败),不进卡片顶部的重试条。
  const save = useCallback(async (patch, { inline = false } = {}) => {
    setSaving(true);
    try {
      applyStatus(await api.setIosSimulatorConfig(patch));
      if (mountedRef.current) setError(null);
      return { ok: true, invalid: false, message: '' };
    } catch (saveError) {
      const text = iosSimulatorErrorMessage(saveError, 'save');
      const invalid = saveError?.code === 'XCODE_INVALID';
      if (mountedRef.current && !(inline && invalid)) setError({ action: 'save', patch, text });
      return { ok: false, invalid, message: text };
    } finally {
      if (mountedRef.current) setSaving(false);
    }
  }, [applyStatus]);

  const supported = status?.supported === true;

  const loadDevices = useCallback(async () => {
    setDevicesLoading(true);
    try {
      const payload = await api.listIosSimulatorDevices();
      if (!mountedRef.current) return;
      setDevices(normalizeSimulatorDevices(payload));
      setDevicesError(String(payload?.error || ''));
    } catch (devicesLoadError) {
      if (mountedRef.current) setDevicesError(iosSimulatorErrorMessage(devicesLoadError, 'device'));
    } finally {
      if (mountedRef.current) setDevicesLoading(false);
    }
  }, []);

  useEffect(() => {
    if (expanded && supported) void loadDevices();
  }, [expanded, loadDevices, supported, status?.xcode?.selected?.app_path]);

  const deviceAction = useCallback(async (device, action, body) => {
    setDeviceBusy(`${device.udid}:${action}`);
    try {
      const payload = await api.iosSimulatorDeviceAction(device.udid, action, body);
      if (mountedRef.current && Array.isArray(payload?.devices)) setDevices(normalizeSimulatorDevices(payload));
    } catch (actionError) {
      toast({ kind: 'err', text: iosSimulatorErrorMessage(actionError, 'device') });
    } finally {
      if (mountedRef.current) setDeviceBusy('');
    }
  }, []);

  const inspect = useCallback(async () => {
    const path = customPath.trim();
    if (!path) return;
    setInspecting(true);
    try {
      const result = await api.inspectIosSimulatorXcode(path);
      if (mountedRef.current) setInspectResult(result);
    } catch (inspectError) {
      if (mountedRef.current) setInspectResult({ ok: false, message: iosSimulatorErrorMessage(inspectError, 'device') });
    } finally {
      if (mountedRef.current) setInspecting(false);
    }
  }, [customPath]);

  const browse = useCallback(async () => {
    try {
      const path = await pickXcodePath();
      if (!path || !mountedRef.current) return;
      customPathTouchedRef.current = true;
      setCustomPath(path);
      setInspectResult(null);
    } catch (pickError) {
      toast({ kind: 'err', text: pickError?.message || '选择器不可用' });
    }
  }, []);

  const saveCustomPath = useCallback(async () => {
    const path = customPath.trim();
    if (!path) return;
    const result = await save({ xcode_path: path }, { inline: true });
    if (!mountedRef.current) return;
    if (result.ok) {
      customPathTouchedRef.current = false;
      setInspectResult(null);
    } else if (result.invalid) {
      setInspectResult({ ok: false, message: result.message });
    }
  }, [customPath, save]);

  const choice = status ? selectedXcodeChoice(status) : IOS_SIMULATOR_AUTO_XCODE;
  const installs = xcodeInstalls(status);
  const notices = iosSimulatorNotices(status);
  const selectedInstall = status?.xcode?.selected || null;
  const inspectView = inspectResultView(inspectResult);
  const busy = loading || saving;
  const config = status?.config || {};
  const qualityOptions = iosSimulatorQualityOptions();

  return (
    <div className="bg-surface border border-border mb-2" aria-busy={busy} data-ios-simulator-settings="true">
      <div className="flex items-center gap-3 px-3.5 py-3 border border-border">
        <div data-settings-surface="true" className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <IosSimulatorIcon size={20} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-normal">iOS 模拟器</div>
          <div className="text-[11px] text-fg-mute mt-0.5">开启后，Agent 可启动、查看并操作 Xcode 中的 iOS 模拟器；你也可以在预览面板中实时观看和操作</div>
          {status && !supported && <div className="text-[11px] text-fg-mute mt-0.5">iOS 模拟器仅在 macOS 上可用</div>}
        </div>
        <button type="button" className={LINK_BUTTON}
          disabled={!status || loading || !supported} aria-expanded={expanded} aria-controls={configId}
          onClick={() => setExpanded((value) => !value)}>{expanded ? '收起' : '配置'}</button>
        <Toggle on={supported && status?.enabled === true} disabled={!status || !supported || busy}
          ariaLabel="启用 iOS 模拟器" onChange={(enabled) => { void save({ enabled }); }} />
      </div>

      {error && <div role="alert" className="px-3.5 py-2 text-[12px] text-danger">
        {error.text}
        <button type="button" disabled={busy} className="ml-2 hover:underline"
          onClick={() => { void (error.action === 'load' ? load() : save(error.patch)); }}>重试</button>
      </div>}

      {expanded && supported && <div id={configId}>
        <div className="border-t border-border px-3.5 py-3">
          <div className="flex items-center justify-between gap-3 mb-1">
            <div className="text-[13px] font-normal text-fg">Xcode</div>
            <button type="button" disabled={rescanning || busy} className={clsx(LINK_BUTTON, 'inline-flex items-center gap-1')}
              onClick={() => { void load({ refresh: true }); }}>
              <RefreshIcon size={12} className={clsx(rescanning && 'animate-spin')} />
              {rescanning ? '正在扫描' : '重新扫描'}
            </button>
          </div>
          <p className="text-[11px] text-fg-mute mb-2">
            自动选择会依次查看 DEVELOPER_DIR、xcode-select、应用程序目录与 Spotlight；Xcode 不在“应用程序”中时可指定路径
          </p>
          {notices.map((notice) => (
            <p key={notice.key} role={notice.kind === 'info' ? undefined : 'status'}
              className={clsx('text-[11px] mb-2 [overflow-wrap:anywhere]', NOTICE_TONE[notice.kind])}>
              {notice.text}
            </p>
          ))}
          <div role="radiogroup" aria-label="Xcode">
            <XcodeOption
              name={radioName}
              value={IOS_SIMULATOR_AUTO_XCODE}
              checked={choice === IOS_SIMULATOR_AUTO_XCODE}
              disabled={busy}
              title="自动选择"
              detail={choice === IOS_SIMULATOR_AUTO_XCODE && selectedInstall
                ? `当前使用 ${xcodeInstallTitle(selectedInstall)} · ${selectedInstall.app_path}`
                : '使用发现到的最合适的 Xcode'}
              onSelect={() => { void save({ xcode_path: '' }); }}
            />
            {installs.map((install) => (
              <XcodeOption
                key={install.app_path}
                name={radioName}
                value={install.app_path}
                checked={choice === install.app_path}
                disabled={busy}
                title={xcodeInstallTitle(install)}
                detail={install.app_path}
                tags={xcodeSourceLabels(install)}
                badge={isSelectedXcodeInstall(status, install) ? '使用中' : ''}
                onSelect={(path) => { void save({ xcode_path: path }); }}
              />
            ))}
            {choice === IOS_SIMULATOR_CUSTOM_XCODE && (
              <XcodeOption
                name={radioName}
                value={IOS_SIMULATOR_CUSTOM_XCODE}
                checked
                disabled={busy}
                title="自定义路径"
                detail={String(config.xcode_path || '')}
                badge={selectedInstall ? '使用中' : ''}
              />
            )}
          </div>
          <label htmlFor={`${configId}-xcode-path`} className="block text-[12px] text-fg-mute mt-3 mb-1">指定 Xcode 路径</label>
          <div className="flex flex-wrap items-center gap-2">
            <input
              id={`${configId}-xcode-path`}
              value={customPath}
              placeholder="/Applications/Xcode.app"
              spellCheck="false"
              disabled={busy}
              className="flex-1 min-w-[220px] h-7 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
              onChange={(event) => {
                customPathTouchedRef.current = true;
                setCustomPath(event.target.value);
                setInspectResult(null);
              }}
              onKeyDown={(event) => { if (event.key === 'Enter') void saveCustomPath(); }}
            />
            <button type="button" className={LINK_BUTTON} disabled={busy} onClick={() => { void browse(); }}>浏览…</button>
            <button type="button" className={LINK_BUTTON} disabled={busy || inspecting || !customPath.trim()}
              onClick={() => { void inspect(); }}>{inspecting ? '正在检查' : '检查'}</button>
            <button type="button" className="px-3 py-1 bg-accent text-white rounded text-[12px] disabled:opacity-60"
              disabled={busy || !customPath.trim()} onClick={() => { void saveCustomPath(); }}>保存</button>
          </div>
          {inspectView && (
            <p role="status" className={clsx('text-[11px] mt-1.5 [overflow-wrap:anywhere]', inspectView.ok ? 'text-ok' : 'text-danger')}>
              {inspectView.text}
              {inspectView.detail && <span className="text-fg-mute"> · {inspectView.detail}</span>}
            </p>
          )}
        </div>

        <div className="border-t border-border px-3.5 py-3">
          <div className="flex items-center justify-between gap-3 mb-1">
            <div className="text-[13px] font-normal text-fg">模拟器设备</div>
            <button type="button" disabled={devicesLoading} className={clsx(LINK_BUTTON, 'inline-flex items-center gap-1')}
              onClick={() => { void loadDevices(); }}>
              <RefreshIcon size={12} className={clsx(devicesLoading && 'animate-spin')} />
              刷新
            </button>
          </div>
          <p className="text-[11px] text-fg-mute mb-2">Agent 只能操作已允许的模拟器；未允许的设备在首次使用时会请求确认</p>
          {devicesError && <p role="status" className="text-[11px] text-danger mb-2 [overflow-wrap:anywhere]">{devicesError}</p>}
          {!devicesError && !devicesLoading && devices.length === 0 && (
            <p className="text-[11px] text-fg-mute">没有可用的模拟器</p>
          )}
          {devices.map((device) => {
            const booting = deviceBusy === `${device.udid}:boot`;
            const stopping = deviceBusy === `${device.udid}:shutdown`;
            return (
              <div key={device.udid} className="flex items-center gap-3 py-2 border-b border-border last:border-b-0">
                <div className="flex-1 min-w-0">
                  <div className="text-[13px] font-normal text-fg truncate">{device.name}</div>
                  <div className="text-[11px] text-fg-mute mt-0.5 truncate">{simulatorDeviceSubtitle(device)}</div>
                </div>
                <span className="text-[11px] text-fg-mute shrink-0">允许 AI 控制</span>
                <Toggle on={device.allowed} disabled={!!deviceBusy} ariaLabel={`允许 AI 控制 ${device.name}`}
                  onChange={(allowed) => { void deviceAction(device, 'consent', { allowed, name: device.name }); }} />
                {device.booted ? (
                  <button type="button" className={clsx(LINK_BUTTON, 'w-14 text-center')} disabled={!!deviceBusy}
                    onClick={() => { void deviceAction(device, 'shutdown'); }}>{stopping ? '正在关闭' : '关闭'}</button>
                ) : (
                  <button type="button" className={clsx(LINK_BUTTON, 'w-14 text-center text-accent')} disabled={!!deviceBusy}
                    onClick={() => { void deviceAction(device, 'boot'); }}>{booting ? '正在启动' : '启动'}</button>
                )}
              </div>
            );
          })}
        </div>

        <div className="border-t border-border px-3.5 py-3">
          <div className="text-[13px] font-normal text-fg mb-1">实时画面</div>
          <p className="text-[11px] text-fg-mute mb-2">预览面板中模拟器画面的帧率、画质与缩放；数值越高越清晰流畅，占用也越多</p>
          <div className="flex flex-wrap items-center gap-x-5 gap-y-2">
            <label className="flex items-center gap-2 text-[12px] text-fg-2">
              帧率
              <select className={SELECT_CLASS} disabled={busy}
                value={String(nearestOption(config.stream_fps ?? 30, IOS_SIMULATOR_FPS_OPTIONS))}
                onChange={(event) => { void save({ stream_fps: Number(event.target.value) }); }}>
                {IOS_SIMULATOR_FPS_OPTIONS.map((fps) => <option key={fps} value={String(fps)}>{`${fps} fps`}</option>)}
              </select>
            </label>
            <label className="flex items-center gap-2 text-[12px] text-fg-2">
              画质
              <select className={SELECT_CLASS} disabled={busy}
                value={String(nearestOption(config.stream_quality ?? 0.6, qualityOptions))}
                onChange={(event) => { void save({ stream_quality: Number(event.target.value) }); }}>
                {qualityOptions.map((option) => <option key={option.value} value={String(option.value)}>{option.label}</option>)}
              </select>
            </label>
            <label className="flex items-center gap-2 text-[12px] text-fg-2">
              缩放
              <select className={SELECT_CLASS} disabled={busy}
                value={String(nearestOption(config.stream_scale ?? 1, IOS_SIMULATOR_SCALE_OPTIONS))}
                onChange={(event) => { void save({ stream_scale: Number(event.target.value) }); }}>
                {IOS_SIMULATOR_SCALE_OPTIONS.map((scale) => <option key={scale} value={String(scale)}>{iosSimulatorScaleLabel(scale)}</option>)}
              </select>
            </label>
          </div>
        </div>
      </div>}
    </div>
  );
}
