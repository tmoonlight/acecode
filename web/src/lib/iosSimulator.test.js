import assert from 'node:assert/strict';
import {
  IOS_SIMULATOR_TOOL_NAME,
  TOUCH_MOVE_INTERVAL_MS,
  containedImageRect,
  deviceCornerRadius,
  downloadSimulatorBlob,
  initialTouchGesture,
  iosSimulatorEventFromToolItem,
  iosSimulatorHostLikelySupported,
  iosSimulatorTabAction,
  iosSimulatorWsUrl,
  isPasteShortcut,
  keyboardEventToMessage,
  latestIosSimulatorEvent,
  normalizeSimulatorDevices,
  normalizeWheelDelta,
  parseIosSimulatorFrame,
  parseIosSimulatorMessage,
  pastedTextMessage,
  pointerToDevicePoint,
  preferredSimulatorUdid,
  reconnectDelayMs,
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
} from './iosSimulator.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function binaryFrame(header, jpeg = [0xff, 0xd8, 0xff, 0xd9]) {
  const json = new TextEncoder().encode(typeof header === 'string' ? header : JSON.stringify(header));
  const bytes = new Uint8Array(4 + json.byteLength + jpeg.length);
  new DataView(bytes.buffer).setUint32(0, json.byteLength, true);
  bytes.set(json, 4);
  bytes.set(jpeg, 4 + json.byteLength);
  return bytes.buffer;
}

function toolItem({ id = 't1', callId = 'call-1', done = true, success = true, meta, tool = IOS_SIMULATOR_TOOL_NAME } = {}) {
  return {
    kind: 'tool',
    id,
    tool: {
      tool,
      toolCallId: callId,
      isDone: done,
      success,
      metadata: meta === undefined ? null : { ios_simulator: meta },
    },
  };
}

run('binary frames parse the little-endian JSON header and keep the JPEG bytes', () => {
  const frame = parseIosSimulatorFrame(binaryFrame({ type: 'frame', seq: 7, width: 1179, height: 2556, rotation: 0 }));
  assert.equal(frame.seq, 7);
  assert.equal(frame.width, 1179);
  assert.equal(frame.height, 2556);
  assert.equal(frame.rotation, 0);
  assert.deepEqual([...frame.jpeg], [0xff, 0xd8, 0xff, 0xd9]);

  // Uint8Array views with a non-zero offset are accepted too.
  const raw = new Uint8Array(binaryFrame({ type: 'frame', seq: 1, width: 10, height: 20, rotation: 90 }));
  const padded = new Uint8Array(raw.byteLength + 3);
  padded.set(raw, 3);
  assert.equal(parseIosSimulatorFrame(padded.subarray(3)).rotation, 90);

  assert.equal(parseIosSimulatorFrame(new ArrayBuffer(2)), null, 'too short');
  const truncated = new Uint8Array(binaryFrame({ type: 'frame', width: 1, height: 1 })).subarray(0, 8);
  assert.equal(parseIosSimulatorFrame(truncated), null, 'header longer than the payload');
  assert.equal(parseIosSimulatorFrame(binaryFrame('{bad json')), null);
  assert.equal(parseIosSimulatorFrame(binaryFrame({ type: 'device', width: 1, height: 1 })), null, 'not a frame');
  assert.equal(parseIosSimulatorFrame(binaryFrame({ type: 'frame', width: 1, height: 1 }, [])), null, 'no image bytes');
  assert.equal(parseIosSimulatorFrame(binaryFrame({ type: 'frame', width: 0, height: 1 })), null, 'no size');
  assert.equal(parseIosSimulatorFrame('text'), null);
});

run('text messages recognise device, ended and error only', () => {
  const device = { udid: 'U', point_width: 393, point_height: 852 };
  assert.deepEqual(parseIosSimulatorMessage(JSON.stringify({ type: 'device', device })), { type: 'device', device });
  assert.deepEqual(parseIosSimulatorMessage('{"type":"ended","reason":"device shut down"}'), { type: 'ended', reason: 'device shut down' });
  assert.deepEqual(parseIosSimulatorMessage('{"type":"error","message":"no Xcode"}'), { type: 'error', message: 'no Xcode' });
  assert.equal(parseIosSimulatorMessage('{"type":"frame"}'), null);
  assert.equal(parseIosSimulatorMessage('{"type":"device"}'), null);
  assert.equal(parseIosSimulatorMessage('not json'), null);
  assert.equal(parseIosSimulatorMessage(''), null);
});

run('WebSocket URLs follow the page origin and carry the daemon token', () => {
  assert.equal(
    iosSimulatorWsUrl({ udid: 'ABC-1', token: 't k', pageProtocol: 'http:', pageHost: '127.0.0.1:28080' }),
    'ws://127.0.0.1:28080/ws/ios-simulator/ABC-1?token=t%20k',
  );
  assert.equal(
    iosSimulatorWsUrl({ udid: 'ABC', pageProtocol: 'https:', pageHost: 'remote.example' }),
    'wss://remote.example/ws/ios-simulator/ABC',
  );
  assert.equal(
    iosSimulatorWsUrl({ udid: 'a/b', origin: 'http://127.0.0.1:4000', token: 'x' }),
    'ws://127.0.0.1:4000/ws/ios-simulator/a%2Fb?token=x',
  );
  assert.equal(
    iosSimulatorWsUrl({ udid: 'ABC', token: 'x', pageHost: 'h', frames: false }),
    'ws://h/ws/ios-simulator/ABC?token=x&frames=0',
  );
  assert.equal(iosSimulatorWsUrl({ udid: 'ABC', pageHost: 'h', frames: false }), 'ws://h/ws/ios-simulator/ABC?frames=0');
});

run('reconnect backoff doubles from one to eight seconds', () => {
  assert.deepEqual([0, 1, 2, 3, 4, 9].map(reconnectDelayMs), [1000, 2000, 4000, 8000, 8000, 8000]);
  assert.equal(reconnectDelayMs(-3), 1000);
});

run('point size swaps to match the frame orientation', () => {
  const device = { point_width: 393, point_height: 852 };
  assert.deepEqual(simulatorPointSize(device, { width: 1179, height: 2556 }), { width: 393, height: 852 });
  assert.deepEqual(simulatorPointSize(device, { width: 2556, height: 1179 }), { width: 852, height: 393 });
  // Device reported in landscape while the frame is portrait.
  assert.deepEqual(simulatorPointSize({ point_width: 852, point_height: 393 }, { width: 1179, height: 2556 }), { width: 393, height: 852 });
  assert.deepEqual(simulatorPointSize(device, null), { width: 393, height: 852 });
  assert.equal(simulatorPointSize({}, { width: 1, height: 2 }), null);
});

run('pointer positions map through the letterboxed image into device points', () => {
  // A 400x400 element showing a portrait 100x200 image: 200px wide, centred (100px bars).
  const rect = { left: 10, top: 20, width: 400, height: 400 };
  assert.deepEqual(containedImageRect(400, 400, 100, 200), { left: 100, top: 0, width: 200, height: 400 });
  const base = { rect, imageWidth: 100, imageHeight: 200, pointSize: { width: 50, height: 100 } };
  assert.deepEqual(pointerToDevicePoint({ ...base, clientX: 110, clientY: 20 }), { x: 0, y: 0 });
  assert.deepEqual(pointerToDevicePoint({ ...base, clientX: 310, clientY: 420 }), { x: 50, y: 100 });
  assert.deepEqual(pointerToDevicePoint({ ...base, clientX: 210, clientY: 220 }), { x: 25, y: 50 });
  assert.equal(pointerToDevicePoint({ ...base, clientX: 50, clientY: 220 }), null, 'left letterbox');
  assert.equal(pointerToDevicePoint({ ...base, clientX: 360, clientY: 220 }), null, 'right letterbox');
  assert.deepEqual(pointerToDevicePoint({ ...base, clientX: 50, clientY: 500, clamp: true }), { x: 0, y: 100 }, 'drags clamp');
  assert.equal(pointerToDevicePoint({ ...base, clientX: 210, clientY: 220, pointSize: null }), null);
  assert.deepEqual(containedImageRect(0, 10, 1, 1), { left: 0, top: 0, width: 0, height: 0 });
  assert.equal(deviceCornerRadius({ product_family: 'iPhone' }, 200, 400), 24);
  assert.equal(deviceCornerRadius({ product_family: 'iPad' }, 300, 400), 12);
  assert.equal(deviceCornerRadius(null, 0, 400), 0);
});

run('touch gestures throttle moves and always deliver the final up', () => {
  let result = touchGestureDown(initialTouchGesture(), { x: 1, y: 1 }, 1000);
  assert.deepEqual(result.messages, [{ type: 'touch', phase: 'down', x: 1, y: 1 }]);
  result = touchGestureMove(result.state, { x: 2, y: 2 }, 1005);
  assert.deepEqual(result.messages, [], 'inside the throttle window');
  assert.equal(touchGestureMillisUntilFlush(result.state, 1005), TOUCH_MOVE_INTERVAL_MS - 5);
  result = touchGestureMove(result.state, { x: 3, y: 3 }, 1010);
  assert.deepEqual(result.state.pending, { x: 3, y: 3 }, 'only the latest point is kept');
  assert.deepEqual(touchGestureFlush(result.state, 1012).messages, [], 'flush waits for the window');
  result = touchGestureFlush(result.state, 1016);
  assert.deepEqual(result.messages, [{ type: 'touch', phase: 'move', x: 3, y: 3 }]);
  result = touchGestureMove(result.state, { x: 4, y: 4 }, 1040);
  assert.deepEqual(result.messages, [{ type: 'touch', phase: 'move', x: 4, y: 4 }]);
  result = touchGestureMove(result.state, { x: 4, y: 4 }, 1100);
  assert.deepEqual(result.messages, [], 'unchanged points are not resent');
  result = touchGestureMove(result.state, { x: 5, y: 5 }, 1101);
  assert.deepEqual(result.messages, [{ type: 'touch', phase: 'move', x: 5, y: 5 }]);
  result = touchGestureMove(result.state, { x: 6, y: 6 }, 1102);
  result = touchGestureUp(result.state, { x: 7, y: 7 }, 1103);
  assert.deepEqual(result.messages, [
    { type: 'touch', phase: 'move', x: 7, y: 7 },
    { type: 'touch', phase: 'up', x: 7, y: 7 },
  ]);
  assert.equal(result.state.active, false);
  assert.equal(touchGestureMillisUntilFlush(result.state, 2000), -1);

  // Cancel (no up position) releases at the pending point, or the last one sent.
  let cancelled = touchGestureDown(initialTouchGesture(), { x: 9, y: 9 }, 0);
  cancelled = touchGestureMove(cancelled.state, { x: 10, y: 10 }, 1);
  assert.deepEqual(touchGestureUp(cancelled.state, null, 2).messages, [
    { type: 'touch', phase: 'move', x: 10, y: 10 },
    { type: 'touch', phase: 'up', x: 10, y: 10 },
  ]);
  const tap = touchGestureDown(initialTouchGesture(), { x: 9, y: 9 }, 0);
  assert.deepEqual(touchGestureUp(tap.state, null, 2).messages, [{ type: 'touch', phase: 'up', x: 9, y: 9 }]);
  assert.deepEqual(touchGestureUp(initialTouchGesture(), { x: 1, y: 1 }, 0).messages, [], 'no gesture, no up');
  assert.deepEqual(touchGestureMove(initialTouchGesture(), { x: 1, y: 1 }, 0).messages, [], 'no gesture, no move');
  assert.deepEqual(touchGestureDown(initialTouchGesture(), null, 0).messages, []);
});

run('wheel deltas become short clamped drags in the opposite direction', () => {
  const pointSize = { width: 400, height: 800 };
  assert.deepEqual(normalizeWheelDelta({ deltaY: 3, deltaMode: 1 }), { x: 0, y: 48 });
  assert.deepEqual(normalizeWheelDelta({ deltaY: 1, deltaMode: 2 }, 800), { x: 0, y: 800 });
  assert.deepEqual(normalizeWheelDelta({ deltaX: 5, deltaY: -7 }), { x: 5, y: -7 });

  const messages = wheelDragGesture({ point: { x: 200, y: 400 }, delta: { x: 0, y: 100 }, pointSize });
  assert.equal(messages[0].phase, 'down');
  assert.deepEqual(messages[0], { type: 'touch', phase: 'down', x: 200, y: 400 });
  assert.deepEqual(messages.at(-1), { type: 'touch', phase: 'up', x: 200, y: 300 }, 'scrolling down drags the finger up');
  assert.equal(messages.filter((message) => message.phase === 'move').length, 4);
  assert.deepEqual(messages.at(-2), { type: 'touch', phase: 'move', x: 200, y: 300 });

  const clamped = wheelDragGesture({ point: { x: 200, y: 400 }, delta: { x: 0, y: -5000 }, pointSize });
  assert.deepEqual(clamped.at(-1), { type: 'touch', phase: 'up', x: 200, y: 540 }, 'clamped to 35% of the short side');
  const edge = wheelDragGesture({ point: { x: 200, y: 20 }, delta: { x: 0, y: 500 }, pointSize });
  assert.deepEqual(edge.at(-1), { type: 'touch', phase: 'up', x: 200, y: 0 }, 'stays on screen');
  assert.deepEqual(wheelDragGesture({ point: { x: 200, y: 0 }, delta: { x: 0, y: 50 }, pointSize }), [], 'no room to move');
  assert.deepEqual(wheelDragGesture({ point: { x: 1, y: 1 }, delta: { x: 0.2, y: 0.3 }, pointSize }), [], 'sub-point delta');
  assert.deepEqual(wheelDragGesture({ point: null, delta: { x: 0, y: 50 }, pointSize }), []);
});

run('keyboard events map to key and type messages', () => {
  const key = (init) => keyboardEventToMessage({ key: '', ...init });
  assert.deepEqual(key({ key: 'Enter' }), { type: 'key', name: 'return' });
  assert.deepEqual(key({ key: 'Backspace' }), { type: 'key', name: 'delete' });
  assert.deepEqual(key({ key: 'Delete' }), { type: 'key', name: 'forward_delete' });
  assert.deepEqual(key({ key: 'Tab' }), { type: 'key', name: 'tab' });
  assert.deepEqual(key({ key: 'Tab', shiftKey: true }), { type: 'key', name: 'shift+tab' });
  assert.deepEqual(key({ key: 'Escape' }), { type: 'key', name: 'escape' });
  assert.deepEqual(key({ key: 'ArrowUp' }), { type: 'key', name: 'up' });
  assert.deepEqual(key({ key: 'ArrowLeft', shiftKey: true }), { type: 'key', name: 'shift+left' });
  assert.deepEqual(key({ key: ' ' }), { type: 'type', text: ' ' });
  assert.deepEqual(key({ key: 'a' }), { type: 'type', text: 'a' });
  assert.deepEqual(key({ key: 'A', shiftKey: true }), { type: 'type', text: 'A' });
  assert.deepEqual(key({ key: 'å', altKey: true }), { type: 'type', text: 'å' });
  assert.deepEqual(key({ key: 'a', metaKey: true }), { type: 'key', name: 'cmd+a' });
  assert.deepEqual(key({ key: 'C', metaKey: true }), { type: 'key', name: 'cmd+c' });
  assert.deepEqual(key({ key: 'v', ctrlKey: true }), { type: 'key', name: 'cmd+v' });
  assert.deepEqual(key({ key: 'x', metaKey: true }), { type: 'key', name: 'cmd+x' });
  assert.deepEqual(key({ key: 'z', metaKey: true }), { type: 'key', name: 'cmd+z' });
  assert.deepEqual(key({ key: 'Z', metaKey: true, shiftKey: true }), { type: 'key', name: 'cmd+shift+z' });
  assert.equal(key({ key: 'w', metaKey: true }), null, 'host shortcuts stay with the host');
  assert.equal(key({ key: 'r', ctrlKey: true }), null);
  assert.equal(key({ key: 'a', metaKey: true, altKey: true }), null);
  for (const modifier of ['Shift', 'Control', 'Alt', 'Meta', 'CapsLock']) assert.equal(key({ key: modifier }), null, modifier);
  assert.equal(key({ key: 'F5' }), null);
  assert.equal(key({ key: 'Unidentified' }), null);
  assert.equal(key({ key: 'a', isComposing: true }), null, 'IME composition');
  assert.equal(key({ key: 'Process', keyCode: 229 }), null);
  assert.equal(key({ key: 'Dead' }), null);
  assert.equal(keyboardEventToMessage(null), null);

  assert.equal(isPasteShortcut({ key: 'v', metaKey: true }), true);
  assert.equal(isPasteShortcut({ key: 'V', ctrlKey: true }), true);
  assert.equal(isPasteShortcut({ key: 'v' }), false);
  assert.equal(isPasteShortcut({ key: 'v', metaKey: true, altKey: true }), false);
});

run('pasted text is normalised and capped at the server limit', () => {
  assert.deepEqual(pastedTextMessage('a\r\nb\rc'), { type: 'text', text: 'a\nb\nc' });
  assert.equal(pastedTextMessage(''), null);
  assert.equal(pastedTextMessage(undefined), null);
  const long = pastedTextMessage('汉'.repeat(3000));
  assert.equal(long.text.length, 2666, 'stops before exceeding 8000 UTF-8 bytes');
  assert.ok(new TextEncoder().encode(long.text).byteLength <= 8000);
});

run('transcript tool results become stable simulator events', () => {
  const meta = { action: 'attach', udid: 'U1', name: 'iPhone 17', runtime: 'iOS 26.5', point_width: 402, point_height: 874, rotation: 0 };
  assert.deepEqual(iosSimulatorEventFromToolItem(toolItem({ meta })), {
    key: 'call-1', action: 'attach', udid: 'U1', name: 'iPhone 17', runtime: 'iOS 26.5',
    pointWidth: 402, pointHeight: 874, rotation: 0,
  });
  assert.equal(iosSimulatorEventFromToolItem(toolItem({ meta, done: false })), null, 'still running');
  assert.equal(iosSimulatorEventFromToolItem(toolItem({ meta, success: false })), null, 'failed call');
  assert.equal(iosSimulatorEventFromToolItem(toolItem({ meta, tool: 'bash' })), null);
  assert.equal(iosSimulatorEventFromToolItem(toolItem({})), null, 'no metadata');
  assert.equal(iosSimulatorEventFromToolItem(toolItem({ meta: { udid: 'U1' } })), null, 'no action');
  assert.equal(iosSimulatorEventFromToolItem({ kind: 'msg' }), null);
  // Without a tool call id the persisted message id, then the item id, is the key.
  assert.equal(iosSimulatorEventFromToolItem({ ...toolItem({ meta, callId: '' }), messageId: 'm9' }).key, 'm9');

  const items = [
    toolItem({ id: 'a', callId: 'c1', meta }),
    { kind: 'msg', role: 'assistant', content: 'ok' },
    toolItem({ id: 'b', callId: 'c2', meta: { action: 'tap', udid: 'U1' } }),
    toolItem({ id: 'c', callId: 'c3', done: false, meta: { action: 'detach', udid: 'U1' } }),
  ];
  assert.equal(latestIosSimulatorEvent(items).key, 'c2', 'the running call does not count yet');
  assert.equal(latestIosSimulatorEvent([]), null);
  assert.equal(latestIosSimulatorEvent(null), null);
});

run('simulator events decide whether a tab opens, focuses or closes', () => {
  const attach = { action: 'attach', udid: 'U1', name: 'iPhone 17' };
  assert.deepEqual(iosSimulatorTabAction(attach), { type: 'open', udid: 'U1', title: 'iPhone 17', focus: true });
  assert.deepEqual(iosSimulatorTabAction(attach, { openUdids: ['U1'] }), { type: 'open', udid: 'U1', title: 'iPhone 17', focus: true },
    'attach refocuses an open tab');
  const tap = { action: 'tap', udid: 'U2', name: 'iPad' };
  assert.deepEqual(iosSimulatorTabAction(tap), { type: 'open', udid: 'U2', title: 'iPad', focus: false });
  assert.deepEqual(iosSimulatorTabAction(tap, { openUdids: ['U2'] }), { type: 'none' });
  assert.deepEqual(iosSimulatorTabAction({ action: 'detach', udid: 'U1' }, { openUdids: ['U1'] }), { type: 'close', udid: 'U1' });
  assert.deepEqual(iosSimulatorTabAction({ action: 'detach', udid: 'U1' }), { type: 'none' });
  assert.deepEqual(iosSimulatorTabAction({ action: 'screenshot', udid: '' }), { type: 'none' });
  assert.deepEqual(iosSimulatorTabAction(null), { type: 'none' });
});

run('device lists sort booted first and pick a sensible default', () => {
  const devices = normalizeSimulatorDevices({
    devices: [
      { udid: 'a', name: 'iPhone 16', state: 'Shutdown', runtime: 'iOS 26.5', allowed: true },
      { udid: 'b', name: 'iPhone 17', state: 'Booted', booted: true, runtime: 'iOS 26.5', sessions: ['s1'] },
      { udid: 'c', name: 'iPhone 15', state: 'Shutdown', runtime: 'iOS 9.3' },
      { udid: 'd', name: 'iPad', state: 'Shutdown', runtime: 'iOS 18.0' },
      { name: 'no udid' },
    ],
  });
  assert.deepEqual(devices.map((device) => device.udid), ['b', 'a', 'd', 'c']);
  assert.equal(devices[0].booted, true);
  assert.deepEqual(devices[0].sessions, ['s1']);
  assert.equal(devices[1].allowed, true);
  assert.deepEqual(normalizeSimulatorDevices(null), []);

  assert.equal(preferredSimulatorUdid({ devices, sessionDevice: 'a' }), 'a', 'session binding wins');
  assert.equal(preferredSimulatorUdid({ devices, sessionDevice: 'gone' }), 'b', 'the only booted device');
  assert.equal(preferredSimulatorUdid({ devices: [...devices, { ...devices[0], udid: 'e' }] }), '', 'ambiguous');
  assert.equal(preferredSimulatorUdid({ devices: [] }), '');
});

run('panel copy helpers', () => {
  assert.equal(simulatorStateLabel('Booted'), '已启动');
  assert.equal(simulatorStateLabel('Shutdown'), '已关闭');
  assert.equal(simulatorStateLabel('Weird'), 'Weird');
  assert.equal(simulatorStateLabel(''), '未知');
  assert.equal(simulatorNeedsSettings({ status: { supported: true, xcode: { selected: null } } }), true);
  assert.equal(simulatorNeedsSettings({ status: { supported: true, xcode: { selected: { app_path: '/A' } } } }), false);
  assert.equal(simulatorNeedsSettings({ status: { supported: false } }), false);
  assert.equal(simulatorNeedsSettings({ message: 'Xcode is not installed' }), true);
  assert.equal(simulatorNeedsSettings({ message: 'device is not booted' }), false);
  assert.equal(
    simulatorScreenshotFilename('iPhone 17 Pro', new Date(2026, 9, 16, 8, 5, 9)),
    'iPhone-17-Pro-20261016-080509.png',
  );
  assert.equal(simulatorScreenshotFilename('', new Date(2026, 0, 2, 3, 4, 5)), 'simulator-20260102-030405.png');
});

run('screenshot downloads go through a temporary object URL', () => {
  const events = [];
  const anchor = {
    style: {},
    click: () => events.push(['click', anchor.href, anchor.download]),
    remove: () => events.push(['remove']),
  };
  const doc = { createElement: () => anchor, body: { append: () => events.push(['append']) } };
  const URLApi = { createObjectURL: () => 'blob:1', revokeObjectURL: (url) => events.push(['revoke', url]) };
  let scheduled = null;
  assert.equal(downloadSimulatorBlob({ size: 1 }, 'a.png', { document: doc, URLApi, schedule: (fn) => { scheduled = fn; } }), true);
  assert.deepEqual(events, [['append'], ['click', 'blob:1', 'a.png'], ['remove']]);
  scheduled();
  assert.deepEqual(events.at(-1), ['revoke', 'blob:1']);
  assert.equal(downloadSimulatorBlob(null, 'a.png', { document: doc, URLApi }), false);
});

run('the panel entry only shows on macOS hosts', () => {
  assert.equal(iosSimulatorHostLikelySupported({ __ACECODE_OS__: 'macos' }), true);
  assert.equal(iosSimulatorHostLikelySupported({ __ACECODE_OS__: 'windows', navigator: { platform: 'MacIntel' } }), false);
  assert.equal(iosSimulatorHostLikelySupported({ navigator: { platform: 'MacIntel', userAgent: '' } }), true);
  assert.equal(iosSimulatorHostLikelySupported({ navigator: { platform: 'Win32', userAgent: 'Windows NT' } }), false);
  assert.equal(iosSimulatorHostLikelySupported(undefined), false);
});
