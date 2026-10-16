// iOS 模拟器实时面板的纯逻辑(Agent 工具 ios_simulator + WS /ws/ios-simulator/<udid>)。
//
// DOM / WebSocket / canvas 接线在 components/IosSimulatorPanel.jsx;这里只做可单测的
// 数据塑形:二进制帧解析、显示坐标 → 设备点、手势节流、滚轮 → 拖动、键盘 → 按键消息、
// transcript 工具结果 → 页签动作。

export const IOS_SIMULATOR_TOOL_NAME = 'ios_simulator';

// 拖动时的 move 消息最多约 60 次/秒;最后一个点总会补发,抬起总会发送。
export const TOUCH_MOVE_INTERVAL_MS = 16;
// 断线重连退避:1s → 2s → 4s → 8s 封顶。
export const RECONNECT_BASE_DELAY_MS = 1000;
export const RECONNECT_MAX_DELAY_MS = 8000;

export function reconnectDelayMs(attempt = 0) {
  const n = Math.max(0, Math.floor(Number(attempt) || 0));
  return Math.min(RECONNECT_MAX_DELAY_MS, RECONNECT_BASE_DELAY_MS * (2 ** Math.min(n, 3)));
}

// WS URL:/ws/ios-simulator/<udid>[?token=...]。origin 形如 "http://127.0.0.1:28080"
// 或空(同源,用页面自身的 protocol/host)—— 与控制台 ptyWsUrl 同一套规则。
export function iosSimulatorWsUrl({ udid, token = '', origin = '', pageProtocol = 'http:', pageHost = '' } = {}) {
  let proto;
  let host;
  if (origin) {
    proto = origin.startsWith('https') ? 'wss:' : 'ws:';
    host = origin.replace(/^https?:\/\//, '');
  } else {
    proto = pageProtocol === 'https:' ? 'wss:' : 'ws:';
    host = pageHost;
  }
  const query = token ? `?token=${encodeURIComponent(token)}` : '';
  return `${proto}//${host}/ws/ios-simulator/${encodeURIComponent(String(udid || ''))}${query}`;
}

// ---------------------------------------------------------------------------
// 服务端消息
// ---------------------------------------------------------------------------

function toBytes(data) {
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  return null;
}

// 二进制帧:u32 小端 json_length | UTF-8 JSON {type:"frame",seq,width,height,rotation} | JPEG。
// 结构不完整或不是 frame 时返回 null(调用方直接丢弃)。
export function parseIosSimulatorFrame(data) {
  const bytes = toBytes(data);
  if (!bytes || bytes.byteLength < 4) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const headerLength = view.getUint32(0, true);
  if (headerLength <= 0 || 4 + headerLength > bytes.byteLength) return null;
  let header;
  try {
    header = JSON.parse(new TextDecoder().decode(bytes.subarray(4, 4 + headerLength)));
  } catch {
    return null;
  }
  if (!header || header.type !== 'frame') return null;
  const jpeg = bytes.subarray(4 + headerLength);
  const width = Number(header.width);
  const height = Number(header.height);
  if (jpeg.byteLength === 0 || !(width > 0) || !(height > 0)) return null;
  return {
    seq: Number(header.seq) || 0,
    width,
    height,
    rotation: Number(header.rotation) || 0,
    jpeg,
  };
}

// 文本消息:device / ended / error。其它类型或坏 JSON 返回 null。
export function parseIosSimulatorMessage(text) {
  if (typeof text !== 'string' || !text) return null;
  let message;
  try {
    message = JSON.parse(text);
  } catch {
    return null;
  }
  if (!message || typeof message !== 'object') return null;
  if (message.type === 'device' && message.device && typeof message.device === 'object') {
    return { type: 'device', device: message.device };
  }
  if (message.type === 'ended') return { type: 'ended', reason: String(message.reason || '') };
  if (message.type === 'error') return { type: 'error', message: String(message.message || '') };
  return null;
}

// ---------------------------------------------------------------------------
// 几何:显示坐标 → 设备点
// ---------------------------------------------------------------------------

// 当前界面方向下的设备点尺寸。设备报告的 point_width/height 是竖屏口径;帧已经按
// 界面方向旋转好,帧是横的而设备尺寸是竖的(或反过来)就交换宽高。
export function simulatorPointSize(device, frame) {
  let width = Number(device?.point_width);
  let height = Number(device?.point_height);
  if (!(width > 0) || !(height > 0)) return null;
  const frameWidth = Number(frame?.width);
  const frameHeight = Number(frame?.height);
  if (frameWidth > 0 && frameHeight > 0 && frameWidth !== frameHeight && width !== height) {
    const frameLandscape = frameWidth > frameHeight;
    const pointLandscape = width > height;
    if (frameLandscape !== pointLandscape) [width, height] = [height, width];
  }
  return { width, height };
}

// object-fit: contain 的图像矩形(相对容器左上角)。
export function containedImageRect(boxWidth, boxHeight, imageWidth, imageHeight) {
  const bw = Math.max(0, Number(boxWidth) || 0);
  const bh = Math.max(0, Number(boxHeight) || 0);
  const iw = Number(imageWidth) || 0;
  const ih = Number(imageHeight) || 0;
  if (!(bw > 0) || !(bh > 0) || !(iw > 0) || !(ih > 0)) return { left: 0, top: 0, width: 0, height: 0 };
  const scale = Math.min(bw / iw, bh / ih);
  const width = iw * scale;
  const height = ih * scale;
  return { left: (bw - width) / 2, top: (bh - height) / 2, width, height };
}

function roundPoint(value) {
  return Math.round(value * 100) / 100;
}

// 把元素内的指针位置映射成设备点。rect 是显示元素的 getBoundingClientRect();
// 图像在元素里按 contain 居中。落在黑边里返回 null;clamp=true(拖动中)时改为夹到边界。
export function pointerToDevicePoint({
  clientX,
  clientY,
  rect,
  imageWidth,
  imageHeight,
  pointSize,
  clamp = false,
} = {}) {
  if (!rect || !pointSize || !(pointSize.width > 0) || !(pointSize.height > 0)) return null;
  const image = containedImageRect(rect.width, rect.height, imageWidth, imageHeight);
  if (!(image.width > 0) || !(image.height > 0)) return null;
  let u = (Number(clientX) - rect.left - image.left) / image.width;
  let v = (Number(clientY) - rect.top - image.top) / image.height;
  if (!Number.isFinite(u) || !Number.isFinite(v)) return null;
  if (u < 0 || u > 1 || v < 0 || v > 1) {
    if (!clamp) return null;
    u = Math.min(1, Math.max(0, u));
    v = Math.min(1, Math.max(0, v));
  }
  return { x: roundPoint(u * pointSize.width), y: roundPoint(v * pointSize.height) };
}

// 圆角跟随设备类型:iPhone 的屏幕圆角大,iPad 小。
export function deviceCornerRadius(device, displayWidth, displayHeight) {
  const shortSide = Math.min(Number(displayWidth) || 0, Number(displayHeight) || 0);
  if (!(shortSide > 0)) return 0;
  const family = String(device?.product_family || device?.device_type || '').toLowerCase();
  const ratio = family.includes('ipad') ? 0.04 : 0.12;
  return Math.round(shortSide * ratio);
}

// ---------------------------------------------------------------------------
// 触摸手势:down → 节流的 move → up
// ---------------------------------------------------------------------------

function touchMessage(phase, point) {
  return { type: 'touch', phase, x: point.x, y: point.y };
}

export function initialTouchGesture() {
  return { active: false, lastSentAt: 0, last: null, pending: null };
}

export function touchGestureDown(state, point, now = 0) {
  if (!point) return { state: state || initialTouchGesture(), messages: [] };
  return {
    state: { active: true, lastSentAt: now, last: point, pending: null },
    messages: [touchMessage('down', point)],
  };
}

export function touchGestureMove(state, point, now = 0, intervalMs = TOUCH_MOVE_INTERVAL_MS) {
  if (!state?.active || !point) return { state, messages: [] };
  if (state.last && state.last.x === point.x && state.last.y === point.y) {
    return { state: { ...state, pending: null }, messages: [] };
  }
  if (now - state.lastSentAt >= intervalMs) {
    return {
      state: { ...state, lastSentAt: now, last: point, pending: null },
      messages: [touchMessage('move', point)],
    };
  }
  return { state: { ...state, pending: point }, messages: [] };
}

// 节流窗口到期时补发最后一个被压下的点;调用方用定时器驱动。
export function touchGestureFlush(state, now = 0, intervalMs = TOUCH_MOVE_INTERVAL_MS) {
  if (!state?.active || !state.pending || now - state.lastSentAt < intervalMs) return { state, messages: [] };
  return {
    state: { ...state, lastSentAt: now, last: state.pending, pending: null },
    messages: [touchMessage('move', state.pending)],
  };
}

// 抬起总会发送:落点优先用 up 事件位置,其次是被压下的最后一个点,最后是上次发出的点。
// 落点与上次发出的点不同时先补一个 move,再发 up。
export function touchGestureUp(state, point, now = 0) {
  if (!state?.active) return { state: initialTouchGesture(), messages: [] };
  const final = point || state.pending || state.last;
  const messages = [];
  if (final && state.last && (final.x !== state.last.x || final.y !== state.last.y)) {
    messages.push(touchMessage('move', final));
  }
  if (final) messages.push(touchMessage('up', final));
  return { state: { ...initialTouchGesture(), lastSentAt: now }, messages };
}

export function touchGestureMillisUntilFlush(state, now = 0, intervalMs = TOUCH_MOVE_INTERVAL_MS) {
  if (!state?.active || !state.pending) return -1;
  return Math.max(0, intervalMs - (now - state.lastSentAt));
}

// ---------------------------------------------------------------------------
// 滚轮 → 短拖动
// ---------------------------------------------------------------------------

export function normalizeWheelDelta({ deltaX = 0, deltaY = 0, deltaMode = 0 } = {}, pageSize = 0) {
  let factor = 1;
  if (deltaMode === 1) factor = 16;
  else if (deltaMode === 2) factor = Math.max(1, Number(pageSize) || 0);
  return { x: (Number(deltaX) || 0) * factor, y: (Number(deltaY) || 0) * factor };
}

// 在光标处按下,朝滚动的反方向拖动(滚轮向下 = 内容上移 = 手指上滑),再抬起。
// 拖动距离夹在屏幕短边的 maxRatio 以内,并保证落点仍在屏幕上;中间插入 steps 个
// move,iOS 才会把它识别成滚动而不是点击。位移不足 1 点时不产生手势。
export function wheelDragGesture({ point, delta, pointSize, maxRatio = 0.35, steps = 4 } = {}) {
  if (!point || !delta || !pointSize || !(pointSize.width > 0) || !(pointSize.height > 0)) return [];
  const limit = Math.min(pointSize.width, pointSize.height) * maxRatio;
  const clampAxis = (value) => Math.max(-limit, Math.min(limit, -(Number(value) || 0)));
  let dx = clampAxis(delta.x);
  let dy = clampAxis(delta.y);
  const endX = Math.max(0, Math.min(pointSize.width, point.x + dx));
  const endY = Math.max(0, Math.min(pointSize.height, point.y + dy));
  dx = endX - point.x;
  dy = endY - point.y;
  if (Math.hypot(dx, dy) < 1) return [];
  const start = { x: roundPoint(point.x), y: roundPoint(point.y) };
  const messages = [touchMessage('down', start)];
  const count = Math.max(1, Math.floor(steps));
  for (let i = 1; i <= count; i += 1) {
    messages.push(touchMessage('move', {
      x: roundPoint(point.x + (dx * i) / count),
      y: roundPoint(point.y + (dy * i) / count),
    }));
  }
  messages.push(touchMessage('up', { x: roundPoint(endX), y: roundPoint(endY) }));
  return messages;
}

// ---------------------------------------------------------------------------
// 键盘 → key / type 消息
// ---------------------------------------------------------------------------

const NAMED_KEYS = Object.freeze({
  Enter: 'return',
  Backspace: 'delete',
  Delete: 'forward_delete',
  Tab: 'tab',
  Escape: 'escape',
  ArrowUp: 'up',
  ArrowDown: 'down',
  ArrowLeft: 'left',
  ArrowRight: 'right',
  Home: 'home',
  End: 'end',
  PageUp: 'page_up',
  PageDown: 'page_down',
});

const MODIFIER_KEYS = new Set(['Shift', 'Control', 'Alt', 'Meta', 'CapsLock', 'Fn', 'FnLock', 'OS', 'Hyper', 'Super', 'AltGraph']);
// 只接管编辑类组合键;Cmd+W / Cmd+R / Cmd+Q 等留给宿主,聚焦在模拟器上也能关窗、刷新。
const COMMAND_LETTERS = new Set(['a', 'c', 'v', 'x', 'z']);

export function isComposingKeyEvent(event) {
  return !!event?.isComposing || event?.keyCode === 229 || event?.key === 'Process' || event?.key === 'Dead';
}

// Cmd/Ctrl+V 交给浏览器原生 paste 事件:那样拿到的是宿主剪贴板文本(发 {type:"text"}),
// 而 cmd+v 按键只会粘贴模拟器自己的剪贴板。
export function isPasteShortcut(event) {
  return !!(event && (event.metaKey || event.ctrlKey) && !event.altKey
    && String(event.key || '').toLowerCase() === 'v');
}

export function keyboardEventToMessage(event) {
  if (!event || isComposingKeyEvent(event)) return null;
  const key = String(event.key || '');
  if (!key || MODIFIER_KEYS.has(key)) return null;
  const command = !!(event.metaKey || event.ctrlKey);
  if (command) {
    const letter = key.length === 1 ? key.toLowerCase() : '';
    if (event.altKey || !COMMAND_LETTERS.has(letter)) return null;
    if (event.shiftKey && letter === 'z') return { type: 'key', name: 'cmd+shift+z' };
    return { type: 'key', name: `cmd+${letter}` };
  }
  const named = NAMED_KEYS[key];
  if (named) {
    const shiftable = named === 'tab' || ['up', 'down', 'left', 'right', 'home', 'end'].includes(named);
    return { type: 'key', name: event.shiftKey && shiftable ? `shift+${named}` : named };
  }
  if (key === ' ' || key === 'Spacebar') return { type: 'type', text: ' ' };
  // 单个可打印字符(含 Option 组合出的 å 之类);F 键、媒体键等多字符名一律忽略。
  if ([...key].length === 1) return { type: 'type', text: key };
  return null;
}

export function pastedTextMessage(text) {
  const value = String(text ?? '').replace(/\r\n?/g, '\n');
  if (!value) return null;
  // 服务端单条文本上限 8000 字节;超出部分截掉,避免整条被丢弃。
  const encoder = new TextEncoder();
  if (encoder.encode(value).byteLength <= 8000) return { type: 'text', text: value };
  let out = '';
  let bytes = 0;
  for (const ch of value) {
    const size = encoder.encode(ch).byteLength;
    if (bytes + size > 8000) break;
    out += ch;
    bytes += size;
  }
  return out ? { type: 'text', text: out } : null;
}

// ---------------------------------------------------------------------------
// transcript 工具结果 → 页签动作
// ---------------------------------------------------------------------------

// 一条已完成的 ios_simulator 工具结果。key 用 tool_call_id(实时 tool_end 与历史
// 重读一致),而不是前端分配的条目 id —— 后者每次重载会话都重新编号。
export function iosSimulatorEventFromToolItem(item) {
  if (item?.kind !== 'tool' || item.tool?.tool !== IOS_SIMULATOR_TOOL_NAME) return null;
  if (!item.tool.isDone || item.tool.success === false) return null;
  const meta = item.tool.metadata?.ios_simulator;
  if (!meta || typeof meta !== 'object' || typeof meta.action !== 'string') return null;
  const key = String(item.tool.toolCallId || item.messageId || item.id || '');
  return {
    key,
    action: meta.action,
    udid: String(meta.udid || ''),
    name: String(meta.name || ''),
    runtime: String(meta.runtime || ''),
    pointWidth: Number(meta.point_width) || 0,
    pointHeight: Number(meta.point_height) || 0,
    rotation: Number(meta.rotation) || 0,
  };
}

export function latestIosSimulatorEvent(items = []) {
  const list = Array.isArray(items) ? items : [];
  for (let i = list.length - 1; i >= 0; i -= 1) {
    const event = iosSimulatorEventFromToolItem(list[i]);
    if (event) return event;
  }
  return null;
}

// attach → 打开并聚焦;其它带 udid 的动作 → 尚未打开时补开(不抢焦点);detach → 关闭。
export function iosSimulatorTabAction(event, { openUdids = [] } = {}) {
  if (!event?.udid) return { type: 'none' };
  const open = Array.isArray(openUdids) && openUdids.includes(event.udid);
  if (event.action === 'detach') return open ? { type: 'close', udid: event.udid } : { type: 'none' };
  if (event.action === 'attach') return { type: 'open', udid: event.udid, title: event.name, focus: true };
  if (open) return { type: 'none' };
  return { type: 'open', udid: event.udid, title: event.name, focus: false };
}

// ---------------------------------------------------------------------------
// 设备列表与面板展示
// ---------------------------------------------------------------------------

export function normalizeSimulatorDevices(payload) {
  const devices = Array.isArray(payload?.devices) ? payload.devices : [];
  return devices
    .filter((device) => device && typeof device.udid === 'string' && device.udid)
    .map((device) => ({
      udid: device.udid,
      name: String(device.name || device.udid),
      state: String(device.state || ''),
      booted: device.booted === true || device.state === 'Booted',
      runtime: String(device.runtime || ''),
      deviceType: String(device.device_type || ''),
      allowed: device.allowed === true,
      sessions: Array.isArray(device.sessions) ? device.sessions.map(String) : [],
    }))
    // 已启动的在前;同组按系统版本从新到旧,再按名称。
    .sort((a, b) => (Number(b.booted) - Number(a.booted))
      || b.runtime.localeCompare(a.runtime, 'en', { numeric: true })
      || a.name.localeCompare(b.name, 'en', { numeric: true }));
}

// 手动打开面板时自动选中的设备:会话已绑定的设备优先,否则唯一一台已启动的设备。
export function preferredSimulatorUdid({ devices = [], sessionDevice = '' } = {}) {
  if (sessionDevice && devices.some((device) => device.udid === sessionDevice)) return sessionDevice;
  const booted = devices.filter((device) => device.booted);
  return booted.length === 1 ? booted[0].udid : '';
}

export function simulatorStateLabel(state) {
  switch (String(state || '')) {
    case 'Booted': return '已启动';
    case 'Shutdown': return '已关闭';
    case 'Booting': return '正在启动';
    case 'Shutting Down': return '正在关闭';
    case 'Creating': return '正在创建';
    default: return String(state || '') || '未知';
  }
}

export function simulatorDeviceSubtitle(device) {
  return [device?.runtime, simulatorStateLabel(device?.state)].filter(Boolean).join(' · ');
}

// 连接失败时判断是否该引导去设置页(没有可用 Xcode / 未启用)。
export function simulatorNeedsSettings({ status = null, message = '' } = {}) {
  if (status && status.supported === false) return false;
  if (status && status.xcode && !status.xcode.selected) return true;
  return /xcode|developer dir|xcrun|simctl/i.test(String(message || ''));
}

export function simulatorScreenshotFilename(name = '', date = new Date()) {
  const safe = String(name || 'simulator').replace(/[-\\/:*?"<>|\s]+/g, '_').replace(/^_+|_+$/g, '') || 'simulator';
  const pad = (value) => String(value).padStart(2, '0');
  const stamp = `${date.getFullYear()}${pad(date.getMonth() + 1)}${pad(date.getDate())}`
    + `_${pad(date.getHours())}${pad(date.getMinutes())}${pad(date.getSeconds())}`;
  return `${safe}_${stamp}.png`;
}

export function downloadSimulatorBlob(blob, filename, {
  document: doc = globalThis.document,
  URLApi = globalThis.URL,
  schedule = globalThis.setTimeout?.bind(globalThis),
} = {}) {
  if (!blob || !filename || typeof doc?.createElement !== 'function'
      || typeof URLApi?.createObjectURL !== 'function') return false;
  let url = '';
  let anchor = null;
  try {
    url = URLApi.createObjectURL(blob);
    anchor = doc.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    anchor.style.display = 'none';
    (doc.body || doc.documentElement)?.append?.(anchor);
    anchor.click();
    anchor.remove?.();
    // 给浏览器留出读取 Blob 的时间再释放 URL。
    const revoke = () => { try { URLApi.revokeObjectURL(url); } catch { /* best effort */ } };
    if (typeof schedule === 'function') schedule(revoke, 60000);
    else revoke();
    return true;
  } catch {
    try { anchor?.remove?.(); } catch { /* best effort */ }
    if (url) {
      try { URLApi.revokeObjectURL(url); } catch { /* best effort */ }
    }
    return false;
  }
}

// 面板入口只在 macOS 宿主上露出;daemon 在其它平台上 status 也会报 supported:false。
export function iosSimulatorHostLikelySupported(win = typeof window !== 'undefined' ? window : undefined) {
  if (!win) return false;
  if (win.__ACECODE_OS__ === 'macos') return true;
  if (win.__ACECODE_OS__ === 'windows' || win.__ACECODE_OS__ === 'linux') return false;
  const platform = win.navigator?.platform || '';
  const ua = win.navigator?.userAgent || '';
  return /mac/i.test(platform) || /Macintosh/i.test(ua);
}
