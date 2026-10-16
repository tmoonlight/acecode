import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { getToken } from '../lib/auth.js';
import { clsx } from '../lib/format.js';
import { iconStrokeWidth } from '../lib/interfaceIcons.js';
import {
  containedImageRect,
  deviceCornerRadius,
  downloadSimulatorBlob,
  initialTouchGesture,
  iosSimulatorWsUrl,
  isComposingKeyEvent,
  isPasteShortcut,
  keyboardEventToMessage,
  normalizeSimulatorDevices,
  normalizeWheelDelta,
  parseIosSimulatorFrame,
  parseIosSimulatorMessage,
  pastedTextMessage,
  pointerToDevicePoint,
  preferredSimulatorUdid,
  reconnectDelayMs,
  simulatorDeviceSubtitle,
  simulatorNeedsSettings,
  simulatorPointSize,
  simulatorScreenshotFilename,
  simulatorStateLabel,
  touchGestureDown,
  touchGestureFlush,
  touchGestureMillisUntilFlush,
  touchGestureMove,
  touchGestureUp,
  wheelDragGesture,
} from '../lib/iosSimulator.js';
import { iosSimulatorErrorMessage } from '../lib/iosSimulatorSettings.js';
import { requestOpenSetting } from '../lib/settingsNavigation.js';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

// 图标库里没有手机图形;按功能图标同一套 20 网格与描边规则画一个。
export function IosSimulatorIcon({ size = 16, className = '' }) {
  return (
    <svg
      viewBox="0 0 20 20"
      width={size}
      height={size}
      fill="none"
      stroke="currentColor"
      strokeWidth={iconStrokeWidth(size)}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      className={clsx('shrink-0', className)}
    >
      <rect x="5.5" y="2" width="9" height="16" rx="2.2" />
      <path d="M8.75 15.25h2.5" />
    </svg>
  );
}

function HomeGlyph({ size = 17 }) {
  return (
    <svg viewBox="0 0 20 20" width={size} height={size} fill="none" stroke="currentColor"
      strokeWidth={iconStrokeWidth(size)} strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
      <path d="M3.5 9.25 10 3.75l6.5 5.5" />
      <path d="M5.5 8v8.25h9V8" />
    </svg>
  );
}

// 优先 createImageBitmap(解码不占主线程);老内核退回 Image + object URL,
// 加载完立即释放 URL。
function decodeJpeg(bytes) {
  const blob = new Blob([bytes], { type: 'image/jpeg' });
  if (typeof createImageBitmap === 'function') return createImageBitmap(blob);
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(blob);
    const image = new Image();
    image.onload = () => {
      URL.revokeObjectURL(url);
      resolve(image);
    };
    image.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error('decode failed'));
    };
    image.src = url;
  });
}

const TOOLBAR_BUTTON = 'w-[30px] h-[30px] shrink-0 inline-flex items-center justify-center rounded-md text-fg-2 transition hover:bg-surface-alt hover:text-fg disabled:opacity-40 disabled:hover:bg-transparent';
const PRIMARY_BUTTON = 'inline-flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white transition hover:opacity-90 disabled:opacity-60';
const SECONDARY_BUTTON = 'inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 transition hover:bg-surface-hi hover:text-fg disabled:opacity-60';

function openIosSimulatorSettings() {
  requestOpenSetting({ section: 'tools', label: 'iOS 模拟器' });
}

function StatusCard({ icon, title, detail, busy = false, tone = 'normal', children }) {
  return (
    <div className="flex flex-col items-center justify-center gap-2.5 px-6 py-8 text-center" role={tone === 'error' ? 'alert' : 'status'}>
      {busy ? <span className="ace-spinner" /> : icon}
      {title && <div className="text-[14px] font-semibold text-fg">{title}</div>}
      {detail && <div className={clsx('max-w-[380px] text-[12px] leading-relaxed whitespace-pre-wrap [overflow-wrap:anywhere]',
        tone === 'error' ? 'text-warn' : 'text-fg-mute')}>{detail}</div>}
      {children && <div className="flex flex-wrap items-center justify-center gap-2 mt-1">{children}</div>}
    </div>
  );
}

function DeviceRow({ device, active = false, busy = false, onConnect, onBoot }) {
  return (
    <div className={clsx('flex items-center gap-3 px-3 py-2 rounded-md border mb-1.5',
      active ? 'border-accent bg-accent-bg' : 'border-border bg-surface')}>
      <IosSimulatorIcon size={18} className="text-fg-2" />
      <div className="flex-1 min-w-0">
        <div className="text-[13px] text-fg truncate">{device.name}</div>
        <div className="text-[11px] text-fg-mute truncate">{simulatorDeviceSubtitle(device)}</div>
      </div>
      {device.booted ? (
        <button type="button" className={SECONDARY_BUTTON} onClick={() => onConnect?.(device)}>
          连接
        </button>
      ) : (
        <button type="button" className={SECONDARY_BUTTON} disabled={busy} onClick={() => onBoot?.(device)}>
          {busy && <span className="ace-spinner w-3 h-3" />}
          {busy ? '正在启动' : '启动'}
        </button>
      )}
    </div>
  );
}

export function IosSimulatorPanel({ api, sessionId = '', udid = '', onSelectDevice }) {
  const [status, setStatus] = useState(null);
  const [devices, setDevices] = useState([]);
  const [devicesLoaded, setDevicesLoaded] = useState(false);
  const [devicesError, setDevicesError] = useState('');
  const [sessionDevice, setSessionDevice] = useState('');
  const [bootingUdid, setBootingUdid] = useState('');
  const [conn, setConn] = useState({ phase: 'idle', message: '', retrySeconds: 0 });
  const [device, setDevice] = useState(null);
  const [frameSize, setFrameSize] = useState(null);
  const [stageSize, setStageSize] = useState({ width: 0, height: 0 });
  const [focused, setFocused] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const [capturing, setCapturing] = useState(false);
  const [reconnectNonce, setReconnectNonce] = useState(0);

  const mountedRef = useRef(true);
  const wsRef = useRef(null);
  const canvasRef = useRef(null);
  const stageRef = useRef(null);
  const inputRef = useRef(null);
  const switcherRef = useRef(null);
  const deviceRef = useRef(null);
  const frameRef = useRef(null);
  const gestureRef = useRef(initialTouchGesture());
  const pointerIdRef = useRef(null);
  const flushTimerRef = useRef(null);
  const wheelRef = useRef({ dx: 0, dy: 0, point: null, timer: null });
  const sequenceTimersRef = useRef([]);
  const composingRef = useRef(false);
  const autoPickedRef = useRef(false);
  const onSelectDeviceRef = useRef(onSelectDevice);
  onSelectDeviceRef.current = onSelectDevice;

  useEffect(() => {
    mountedRef.current = true;
    return () => { mountedRef.current = false; };
  }, []);

  const loadDevices = useCallback(async () => {
    try {
      const payload = await api.listIosSimulatorDevices(sessionId);
      if (!mountedRef.current) return;
      setDevices(normalizeSimulatorDevices(payload));
      setDevicesError(String(payload?.error || ''));
      setSessionDevice(String(payload?.session_device || ''));
    } catch (error) {
      if (mountedRef.current) setDevicesError(iosSimulatorErrorMessage(error, 'device'));
    } finally {
      if (mountedRef.current) setDevicesLoaded(true);
    }
  }, [api, sessionId]);

  useEffect(() => {
    let cancelled = false;
    api.getIosSimulatorStatus()
      .then((next) => { if (!cancelled) setStatus(next || null); })
      .catch(() => { /* 状态只用于提示文案;取不到时按连接结果展示 */ });
    void loadDevices();
    return () => { cancelled = true; };
  }, [api, loadDevices]);

  const unsupported = status?.supported === false;

  // 手动打开的选择页:会话已绑定设备或只有一台已启动设备时直接选中。
  useEffect(() => {
    if (udid || !devicesLoaded || autoPickedRef.current) return;
    autoPickedRef.current = true;
    const preferred = preferredSimulatorUdid({ devices, sessionDevice });
    if (preferred) {
      onSelectDeviceRef.current?.(preferred, devices.find((item) => item.udid === preferred)?.name || '');
    }
  }, [devices, devicesLoaded, sessionDevice, udid]);

  const send = useCallback((message) => {
    const ws = wsRef.current;
    if (!message || !ws || ws.readyState !== WebSocket.OPEN) return false;
    ws.send(JSON.stringify(message));
    return true;
  }, []);

  const clearSequence = useCallback(() => {
    sequenceTimersRef.current.forEach((timer) => clearTimeout(timer));
    sequenceTimersRef.current = [];
  }, []);

  // 滚轮手势的消息按 16ms 间隔发出,iOS 才能算出拖动速度。
  const sendSequence = useCallback((messages) => {
    clearSequence();
    messages.forEach((message, index) => {
      const timer = setTimeout(() => {
        sequenceTimersRef.current = sequenceTimersRef.current.filter((item) => item !== timer);
        send(message);
      }, index * 16);
      sequenceTimersRef.current.push(timer);
    });
  }, [clearSequence, send]);

  // 松开前离开(切页签、断线)时补一个 up,避免模拟器里留下按住不放的手指。
  const releaseGesture = useCallback(() => {
    if (flushTimerRef.current) {
      clearTimeout(flushTimerRef.current);
      flushTimerRef.current = null;
    }
    const result = touchGestureUp(gestureRef.current, null, performance.now());
    gestureRef.current = result.state;
    result.messages.forEach(send);
    pointerIdRef.current = null;
  }, [send]);

  // 连接生命周期:device / 帧 / ended / error;ended 与意外断线按 1s→8s 退避自动重连。
  useEffect(() => {
    if (!udid || unsupported) return undefined;
    let disposed = false;
    let attempt = 0;
    let reconnectTimer = null;
    let decoding = false;
    let pendingFrame = null;

    const detach = (ws) => {
      if (!ws) return;
      ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
      try { ws.close(1000); } catch { /* already closed */ }
    };

    const drawFrame = (frame) => {
      if (decoding) {
        pendingFrame = frame;
        return;
      }
      decoding = true;
      decodeJpeg(frame.jpeg).then((image) => {
        if (disposed) {
          image.close?.();
          return;
        }
        const canvas = canvasRef.current;
        if (canvas) {
          if (canvas.width !== frame.width) canvas.width = frame.width;
          if (canvas.height !== frame.height) canvas.height = frame.height;
          canvas.getContext('2d')?.drawImage(image, 0, 0, frame.width, frame.height);
        }
        image.close?.();
        frameRef.current = { width: frame.width, height: frame.height, rotation: frame.rotation };
        setFrameSize((prev) => (prev && prev.width === frame.width && prev.height === frame.height
          && prev.rotation === frame.rotation ? prev : frameRef.current));
      }).catch(() => { /* 丢弃坏帧,下一帧会覆盖 */ }).finally(() => {
        decoding = false;
        const next = pendingFrame;
        pendingFrame = null;
        if (next && !disposed) drawFrame(next);
      });
    };

    const scheduleReconnect = (reason) => {
      const delay = reconnectDelayMs(attempt);
      attempt += 1;
      setConn({ phase: 'ended', message: reason, retrySeconds: Math.round(delay / 1000) });
      reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        if (!disposed) connect();
      }, delay);
    };

    const stop = (ws) => {
      releaseGesture();
      if (wsRef.current === ws) wsRef.current = null;
      detach(ws);
    };

    function connect() {
      setConn({ phase: 'connecting', message: '', retrySeconds: 0 });
      const ws = new WebSocket(iosSimulatorWsUrl({
        udid,
        token: getToken() || '',
        pageProtocol: location.protocol,
        pageHost: location.host,
      }));
      ws.binaryType = 'arraybuffer';
      wsRef.current = ws;
      ws.onmessage = (event) => {
        if (disposed || wsRef.current !== ws) return;
        if (typeof event.data === 'string') {
          const message = parseIosSimulatorMessage(event.data);
          if (message?.type === 'device') {
            deviceRef.current = message.device;
            setDevice(message.device);
          } else if (message?.type === 'ended') {
            stop(ws);
            scheduleReconnect(message.reason || '模拟器画面已结束');
          } else if (message?.type === 'error') {
            stop(ws);
            setConn({ phase: 'error', message: message.message || '无法连接模拟器', retrySeconds: 0 });
            void loadDevices();
          }
          return;
        }
        const frame = parseIosSimulatorFrame(event.data);
        if (!frame) return;
        attempt = 0;
        setConn((prev) => (prev.phase === 'live' ? prev : { phase: 'live', message: '', retrySeconds: 0 }));
        drawFrame(frame);
      };
      ws.onclose = () => {
        if (disposed || wsRef.current !== ws) return;
        stop(ws);
        scheduleReconnect('与模拟器的连接已断开');
      };
    }

    deviceRef.current = null;
    frameRef.current = null;
    setDevice(null);
    setFrameSize(null);
    connect();
    return () => {
      disposed = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      clearSequence();
      const wheel = wheelRef.current;
      if (wheel.timer) clearTimeout(wheel.timer);
      wheelRef.current = { dx: 0, dy: 0, point: null, timer: null };
      const ws = wsRef.current;
      releaseGesture();
      wsRef.current = null;
      detach(ws);
    };
  }, [clearSequence, loadDevices, reconnectNonce, releaseGesture, udid, unsupported]);

  useLayoutEffect(() => {
    const stage = stageRef.current;
    if (!stage) return undefined;
    const update = () => {
      const rect = stage.getBoundingClientRect();
      setStageSize((prev) => (Math.abs(prev.width - rect.width) < 0.5 && Math.abs(prev.height - rect.height) < 0.5
        ? prev : { width: rect.width, height: rect.height }));
    };
    update();
    const observer = new ResizeObserver(update);
    observer.observe(stage);
    return () => observer.disconnect();
  }, [udid, unsupported]);

  const mapPoint = useCallback((event, clamp) => {
    const canvas = canvasRef.current;
    const frame = frameRef.current;
    if (!canvas || !frame) return null;
    return pointerToDevicePoint({
      clientX: event.clientX,
      clientY: event.clientY,
      rect: canvas.getBoundingClientRect(),
      imageWidth: frame.width,
      imageHeight: frame.height,
      pointSize: simulatorPointSize(deviceRef.current, frame),
      clamp,
    });
  }, []);

  const applyGesture = useCallback((result) => {
    gestureRef.current = result.state;
    result.messages.forEach(send);
    if (flushTimerRef.current) {
      clearTimeout(flushTimerRef.current);
      flushTimerRef.current = null;
    }
    const wait = touchGestureMillisUntilFlush(gestureRef.current, performance.now());
    if (wait >= 0) {
      flushTimerRef.current = setTimeout(() => {
        flushTimerRef.current = null;
        applyGesture(touchGestureFlush(gestureRef.current, performance.now()));
      }, wait);
    }
  }, [send]);

  const focusInput = useCallback(() => {
    inputRef.current?.focus({ preventScroll: true });
  }, []);

  const onPointerDown = useCallback((event) => {
    if (event.pointerType === 'mouse' && event.button !== 0) return;
    event.preventDefault();
    focusInput();
    if (pointerIdRef.current !== null) return;
    const point = mapPoint(event, false);
    if (!point) return;
    clearSequence();
    pointerIdRef.current = event.pointerId;
    try { event.currentTarget.setPointerCapture(event.pointerId); } catch { /* capture is best effort */ }
    applyGesture(touchGestureDown(gestureRef.current, point, performance.now()));
  }, [applyGesture, clearSequence, focusInput, mapPoint]);

  const onPointerMove = useCallback((event) => {
    if (pointerIdRef.current !== event.pointerId) return;
    applyGesture(touchGestureMove(gestureRef.current, mapPoint(event, true), performance.now()));
  }, [applyGesture, mapPoint]);

  const onPointerUp = useCallback((event) => {
    if (pointerIdRef.current !== event.pointerId) return;
    const point = event.type === 'pointerup' ? mapPoint(event, true) : null;
    pointerIdRef.current = null;
    try { event.currentTarget.releasePointerCapture?.(event.pointerId); } catch { /* already released */ }
    applyGesture(touchGestureUp(gestureRef.current, point, performance.now()));
  }, [applyGesture, mapPoint]);

  // React 的 onWheel 是被动监听,无法阻止默认滚动;这里挂原生非被动监听。连续的
  // 滚轮事件攒 50ms 合成一次短拖动,避免触控板把模拟器淹没在手势里。
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const flush = () => {
      const wheel = wheelRef.current;
      wheel.timer = null;
      if (pointerIdRef.current !== null || sequenceTimersRef.current.length > 0) {
        wheel.timer = setTimeout(flush, 50);
        return;
      }
      const frame = frameRef.current;
      const pointSize = simulatorPointSize(deviceRef.current, frame);
      const messages = wheelDragGesture({ point: wheel.point, delta: { x: wheel.dx, y: wheel.dy }, pointSize });
      wheelRef.current = { dx: 0, dy: 0, point: null, timer: null };
      if (messages.length) sendSequence(messages);
    };
    const onWheel = (event) => {
      event.preventDefault();
      if (event.ctrlKey) return;
      const point = mapPoint(event, false);
      const pointSize = simulatorPointSize(deviceRef.current, frameRef.current);
      if (!point || !pointSize) return;
      const delta = normalizeWheelDelta(event, pointSize.height);
      const wheel = wheelRef.current;
      if (!wheel.point) wheel.point = point;
      wheel.dx += delta.x;
      wheel.dy += delta.y;
      if (!wheel.timer) wheel.timer = setTimeout(flush, 50);
    };
    canvas.addEventListener('wheel', onWheel, { passive: false });
    return () => canvas.removeEventListener('wheel', onWheel);
  }, [mapPoint, sendSequence, udid, unsupported]);

  const onKeyDown = useCallback((event) => {
    if (composingRef.current || isComposingKeyEvent(event)) return;
    // Cmd/Ctrl+V 放行给原生 paste 事件,拿宿主剪贴板文本。
    if (isPasteShortcut(event)) {
      event.stopPropagation();
      return;
    }
    const message = keyboardEventToMessage(event);
    if (!message) return;
    event.preventDefault();
    event.stopPropagation();
    send(message);
  }, [send]);

  const onPaste = useCallback((event) => {
    const message = pastedTextMessage(event.clipboardData?.getData('text/plain'));
    event.preventDefault();
    if (message) send(message);
  }, [send]);

  // 输入法(中文等)合成结束后把整段文字作为文本发出;服务端对非 ASCII 走模拟器剪贴板。
  const onCompositionEnd = useCallback((event) => {
    composingRef.current = false;
    const message = pastedTextMessage(event.data);
    if (message) send(message);
    if (inputRef.current) inputRef.current.value = '';
  }, [send]);

  const onInput = useCallback((event) => {
    if (composingRef.current) return;
    const value = event.currentTarget.value;
    event.currentTarget.value = '';
    const message = pastedTextMessage(value);
    if (message) send(message);
  }, [send]);

  const reconnect = useCallback(() => {
    setReconnectNonce((value) => value + 1);
  }, []);

  const bootDevice = useCallback(async (target) => {
    if (!target?.udid || bootingUdid) return;
    setBootingUdid(target.udid);
    try {
      const payload = await api.iosSimulatorDeviceAction(target.udid, 'boot');
      if (!mountedRef.current) return;
      if (Array.isArray(payload?.devices)) setDevices(normalizeSimulatorDevices(payload));
      if (target.udid === udid) reconnect();
      else onSelectDeviceRef.current?.(target.udid, target.name);
    } catch (error) {
      if (mountedRef.current) toast({ kind: 'err', text: iosSimulatorErrorMessage(error, 'device') });
    } finally {
      if (mountedRef.current) setBootingUdid('');
    }
  }, [api, bootingUdid, reconnect, udid]);

  const captureScreenshot = useCallback(async () => {
    if (!udid || capturing) return;
    setCapturing(true);
    try {
      const blob = await api.iosSimulatorScreenshot(udid);
      const name = deviceRef.current?.name || devices.find((item) => item.udid === udid)?.name || '';
      if (!downloadSimulatorBlob(blob, simulatorScreenshotFilename(name))) {
        toast({ kind: 'err', text: '当前环境无法保存截图' });
      }
    } catch (error) {
      toast({ kind: 'err', text: iosSimulatorErrorMessage(error, 'device') || '截图失败' });
    } finally {
      if (mountedRef.current) setCapturing(false);
    }
  }, [api, capturing, devices, udid]);

  const selectDevice = useCallback((target) => {
    setMenuOpen(false);
    if (!target?.udid) return;
    if (!target.booted) {
      void bootDevice(target);
      return;
    }
    if (target.udid !== udid) onSelectDeviceRef.current?.(target.udid, target.name);
  }, [bootDevice, udid]);

  const listedDevice = devices.find((item) => item.udid === udid) || null;
  const displayName = device?.name || listedDevice?.name || '';
  const displayRuntime = device?.runtime || listedDevice?.runtime || '';
  const displayState = device?.state || listedDevice?.state || '';
  const needsSettings = simulatorNeedsSettings({ status, message: conn.message || devicesError });

  const displayRect = useMemo(() => (frameSize
    ? containedImageRect(stageSize.width, stageSize.height, frameSize.width, frameSize.height)
    : null), [frameSize, stageSize.height, stageSize.width]);
  const cornerRadius = displayRect ? deviceCornerRadius(device, displayRect.width, displayRect.height) : 0;

  if (unsupported) {
    return (
      <div className="flex-1 min-h-0 flex items-center justify-center bg-surface" data-ios-simulator-panel="unsupported">
        <StatusCard icon={<IosSimulatorIcon size={44} className="text-fg-mute" />}
          title="iOS 模拟器仅在 macOS 上可用" detail="请在安装了 Xcode 的 Mac 上使用 ACECode" />
      </div>
    );
  }

  // 选择页:手动打开且还没有选定设备。
  if (!udid) {
    return (
      <div className="flex-1 min-h-0 overflow-y-auto bg-surface" data-ios-simulator-panel="chooser">
        <div className="max-w-[460px] mx-auto px-4 py-6">
          <div className="flex items-center justify-between gap-3 mb-1">
            <div className="text-[14px] font-semibold text-fg">选择模拟器</div>
            <button type="button" className={TOOLBAR_BUTTON} title="刷新设备列表" aria-label="刷新设备列表"
              onClick={() => { void loadDevices(); }}>
              <VsIcon name="refresh" size={15} />
            </button>
          </div>
          <p className="text-[12px] text-fg-mute mb-3">连接一台已启动的模拟器，或先启动一台。</p>
          {!devicesLoaded && <StatusCard busy title="" detail="正在读取模拟器列表" />}
          {devicesLoaded && devicesError && (
            <StatusCard tone="error" icon={<VsIcon name="warning" size={32} className="text-warn" />}
              title="无法读取模拟器" detail={devicesError}>
              {needsSettings && <button type="button" className={PRIMARY_BUTTON} onClick={openIosSimulatorSettings}>打开设置</button>}
            </StatusCard>
          )}
          {devicesLoaded && !devicesError && devices.length === 0 && (
            <StatusCard icon={<IosSimulatorIcon size={36} className="text-fg-mute" />}
              title="没有可用的模拟器" detail="请在 Xcode 中下载 iOS 模拟器运行时后重试" />
          )}
          {devices.map((item) => (
            <DeviceRow key={item.udid} device={item} active={item.udid === sessionDevice}
              busy={bootingUdid === item.udid}
              onConnect={(target) => onSelectDeviceRef.current?.(target.udid, target.name)}
              onBoot={bootDevice} />
          ))}
        </div>
      </div>
    );
  }

  const live = conn.phase === 'live' && !!frameSize;
  const notBooted = !!listedDevice && !listedDevice.booted;

  return (
    <div className="flex-1 min-w-0 min-h-0 flex flex-col bg-surface overflow-hidden" data-ios-simulator-panel="stream"
      data-ios-simulator-phase={conn.phase}>
      <div className="min-h-[44px] shrink-0 flex items-center gap-1 px-2 py-1.5 border-b border-border bg-surface-hi">
        <button
          ref={switcherRef}
          type="button"
          className="min-w-0 h-[30px] flex items-center gap-2 px-2 rounded-md text-left transition hover:bg-surface-alt"
          aria-haspopup="menu"
          aria-expanded={menuOpen}
          title="切换模拟器"
          onClick={() => {
            setMenuOpen((open) => !open);
            void loadDevices();
          }}
        >
          <IosSimulatorIcon size={16} className="text-fg-2" />
          <span className="min-w-0 flex items-baseline gap-1.5">
            <span className="min-w-[3em] text-[12.5px] font-medium text-fg truncate">{displayName || '模拟器'}</span>
            {(displayRuntime || displayState) && (
              <span className="min-w-0 shrink-[4] text-[11px] text-fg-mute truncate">
                {[displayRuntime, displayState ? simulatorStateLabel(displayState) : ''].filter(Boolean).join(' · ')}
              </span>
            )}
          </span>
          <VsIcon name="expandDown" size={12} className="text-fg-mute shrink-0" />
        </button>
        {bootingUdid && <span className="ace-spinner w-3 h-3 ml-1" title="正在启动模拟器" />}
        <div className="flex-1" />
        <button type="button" className={TOOLBAR_BUTTON} disabled={!live} title="主屏幕" aria-label="主屏幕"
          onClick={() => send({ type: 'button', name: 'home' })}>
          <HomeGlyph size={17} />
        </button>
        <button type="button" className={TOOLBAR_BUTTON} disabled={!live} title="锁屏" aria-label="锁屏"
          onClick={() => send({ type: 'button', name: 'lock' })}>
          <VsIcon name="lock" size={16} />
        </button>
        <button type="button" className={TOOLBAR_BUTTON} disabled={!live || capturing} title="截图" aria-label="截图"
          onClick={() => { void captureScreenshot(); }}>
          {capturing ? <span className="ace-spinner w-3 h-3" /> : <VsIcon name="Image" size={16} />}
        </button>
        <button type="button" className={TOOLBAR_BUTTON} title="重新连接" aria-label="重新连接" onClick={reconnect}>
          <VsIcon name="refresh" size={15} />
        </button>
      </div>
      {menuOpen && (
        <AnchoredMenu
          anchorRef={switcherRef}
          onClose={() => setMenuOpen(false)}
          width={300}
          maxHeightRatio={0.6}
          role="menu"
          aria-label="切换模拟器"
          className="bg-surface border border-border ace-shadow rounded-lg z-50 py-1"
        >
          {devices.length === 0 && (
            <div className="px-3 py-2 text-[12px] text-fg-mute">{devicesError || '没有可用的模拟器'}</div>
          )}
          {devices.map((item) => (
            <button
              key={item.udid}
              type="button"
              role="menuitemradio"
              aria-checked={item.udid === udid}
              disabled={bootingUdid === item.udid}
              className={clsx('w-full flex items-center gap-2 text-left px-2.5 py-1.5 transition-colors',
                item.udid === udid ? 'bg-accent/10 text-accent' : 'text-fg hover:bg-surface-hi')}
              onClick={() => selectDevice(item)}
            >
              <span className="flex-1 min-w-0">
                <span className="block text-[12.5px] truncate">{item.name}</span>
                <span className="block text-[11px] text-fg-mute truncate">{simulatorDeviceSubtitle(item)}</span>
              </span>
              {bootingUdid === item.udid
                ? <span className="ace-spinner w-3 h-3" />
                : !item.booted && <span className="text-[11px] text-accent shrink-0">启动</span>}
            </button>
          ))}
        </AnchoredMenu>
      )}
      <div
        className={clsx('relative flex-1 min-h-0 m-2 rounded-lg bg-surface-alt overflow-hidden outline-none',
          focused && live && 'ring-2 ring-accent')}
      >
        <textarea
          ref={inputRef}
          className="sr-only"
          aria-label="模拟器键盘输入"
          autoCapitalize="off"
          autoCorrect="off"
          autoComplete="off"
          spellCheck="false"
          tabIndex={live ? 0 : -1}
          onKeyDown={onKeyDown}
          onPaste={onPaste}
          onInput={onInput}
          onCompositionStart={() => { composingRef.current = true; }}
          onCompositionEnd={onCompositionEnd}
          onFocus={() => setFocused(true)}
          onBlur={() => {
            setFocused(false);
            releaseGesture();
          }}
        />
        <div ref={stageRef} className="absolute inset-3">
          <canvas
            ref={canvasRef}
            aria-label={displayName ? `${displayName} 屏幕` : '模拟器屏幕'}
            className="absolute touch-none select-none shadow-[0_0_0_1px_var(--ace-border)]"
            style={displayRect ? {
              left: displayRect.left,
              top: displayRect.top,
              width: displayRect.width,
              height: displayRect.height,
              borderRadius: cornerRadius,
              visibility: live ? 'visible' : 'hidden',
              cursor: 'pointer',
            } : { visibility: 'hidden', width: 0, height: 0 }}
            onPointerDown={onPointerDown}
            onPointerMove={onPointerMove}
            onPointerUp={onPointerUp}
            onPointerCancel={onPointerUp}
            onLostPointerCapture={onPointerUp}
            onContextMenu={(event) => event.preventDefault()}
          />
        </div>
        {!live && (
          <div className="absolute inset-0 flex items-center justify-center bg-surface-alt">
            {conn.phase === 'error' ? (
              <StatusCard tone="error" icon={<VsIcon name="warning" size={36} className="text-warn" />}
                title={notBooted ? '模拟器未启动' : '无法连接模拟器'} detail={conn.message}>
                {notBooted && (
                  <button type="button" className={PRIMARY_BUTTON} disabled={!!bootingUdid}
                    onClick={() => { void bootDevice(listedDevice); }}>
                    {bootingUdid === udid && <span className="ace-spinner w-3 h-3" />}
                    {bootingUdid === udid ? '正在启动' : '启动'}
                  </button>
                )}
                {needsSettings && <button type="button" className={PRIMARY_BUTTON} onClick={openIosSimulatorSettings}>打开设置</button>}
                <button type="button" className={SECONDARY_BUTTON} onClick={reconnect}>重新连接</button>
              </StatusCard>
            ) : conn.phase === 'ended' ? (
              <StatusCard icon={<IosSimulatorIcon size={40} className="text-fg-mute" />}
                title="模拟器画面已中断"
                detail={[conn.message, conn.retrySeconds ? `${conn.retrySeconds} 秒后自动重新连接` : ''].filter(Boolean).join('\n')}>
                {notBooted && (
                  <button type="button" className={PRIMARY_BUTTON} disabled={!!bootingUdid}
                    onClick={() => { void bootDevice(listedDevice); }}>
                    {bootingUdid === udid && <span className="ace-spinner w-3 h-3" />}
                    {bootingUdid === udid ? '正在启动' : '启动'}
                  </button>
                )}
                <button type="button" className={SECONDARY_BUTTON} onClick={reconnect}>重新连接</button>
              </StatusCard>
            ) : (
              <StatusCard busy title={bootingUdid === udid ? '正在启动模拟器' : '正在连接模拟器'}
                detail={displayName ? `${displayName}${displayRuntime ? ` · ${displayRuntime}` : ''}` : ''} />
            )}
          </div>
        )}
      </div>
      <div className="shrink-0 px-3 pb-2 text-[11px] text-fg-mute text-center truncate" aria-live="polite">
        {focused && live ? '键盘输入会发送到模拟器，点击面板外任意位置结束' : '点击屏幕后可直接用键盘输入，支持粘贴文本'}
      </div>
    </div>
  );
}
