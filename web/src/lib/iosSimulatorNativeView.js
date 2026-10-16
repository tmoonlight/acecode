// Desktop (macOS) native iOS Simulator view: the shell shows the simulator's
// IOSurface in a layer over the panel's screen area, so frames skip JPEG
// encoding. The canvas stays underneath as the input target; the WebSocket
// keeps carrying touches and keys, with the JPEG stream switched off while
// the native view renders.

export const IOS_SIMULATOR_VIEW_STATE_EVENT = 'acecode:ios-simulator-view-state';

export function hasNativeIosSimulatorView(win = globalThis.window) {
  return win?.__ACECODE_DESKTOP_SHELL__ === true
    && win?.__ACECODE_OS__ === 'macos'
    && typeof win.aceDesktop_iosSimulatorViewSetLayout === 'function'
    && typeof win.aceDesktop_iosSimulatorViewUnmount === 'function';
}

export function parseNativeViewResult(raw) {
  if (raw && typeof raw === 'object') return raw;
  try {
    const parsed = JSON.parse(String(raw ?? ''));
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    return {};
  }
}

function finite(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

// Layout request in CSS pixels, which the macOS shell uses as points.
export function nativeSimulatorLayout({
  udid = '',
  developerDir = '',
  rect = null,
  visible = false,
  cornerRadius = 0,
  occlusionRects = [],
} = {}) {
  return {
    udid: String(udid || ''),
    developer_dir: String(developerDir || ''),
    x: Math.round(finite(rect?.left)),
    y: Math.round(finite(rect?.top)),
    width: Math.max(0, Math.round(finite(rect?.width))),
    height: Math.max(0, Math.round(finite(rect?.height))),
    visible: !!visible,
    corner_radius: Math.max(0, finite(cornerRadius)),
    occlusion_rects: Array.isArray(occlusionRects) ? occlusionRects : [],
  };
}

export function nativeSimulatorLayoutSignature(layout = {}) {
  const rects = (Array.isArray(layout.occlusion_rects) ? layout.occlusion_rects : [])
    .map((rect) => [rect.x, rect.y, rect.width, rect.height].join(','))
    .join(';');
  return [layout.udid, layout.developer_dir, layout.x, layout.y, layout.width, layout.height,
    layout.visible ? 1 : 0, layout.corner_radius, rects].join('|');
}

// True when the native view is drawing this device right now.
export function nativeViewRendering(state, udid) {
  return !!state && state.rendering === true && !!udid && state.udid === udid;
}

// Image size and rotation for pointer mapping, as a JPEG frame would carry:
// the framebuffer is portrait, so quarter turns swap the sides.
export function nativeFrameFromState(state) {
  const width = finite(state?.pixel_width);
  const height = finite(state?.pixel_height);
  if (width <= 0 || height <= 0) return null;
  const rotation = [90, 180, 270].includes(finite(state?.rotation)) ? finite(state.rotation) : 0;
  const quarter = rotation === 90 || rotation === 270;
  return { width: quarter ? height : width, height: quarter ? width : height, rotation };
}

// Developer directory of the Xcode the daemon selected (status payload).
export function simulatorDeveloperDir(status) {
  return String(status?.xcode?.selected?.developer_dir || status?.helper?.developer_dir || '');
}

// The shell has one native view; the panel that placed it last owns it, and
// only the owner may take it down (another panel may have taken over).
let activeOwner = null;

export async function setNativeSimulatorLayout(layout, owner = null, win = globalThis.window) {
  if (!hasNativeIosSimulatorView(win)) return { ok: false, supported: false };
  activeOwner = owner;
  try {
    return parseNativeViewResult(await win.aceDesktop_iosSimulatorViewSetLayout(layout));
  } catch (error) {
    return { ok: false, error: String(error?.message || error || 'native view failed') };
  }
}

export async function unmountNativeSimulatorView(owner = null, win = globalThis.window) {
  if (!hasNativeIosSimulatorView(win)) return { ok: false, supported: false };
  if (owner !== null && activeOwner !== owner) return { ok: true, skipped: true };
  activeOwner = null;
  try {
    return parseNativeViewResult(await win.aceDesktop_iosSimulatorViewUnmount());
  } catch (error) {
    return { ok: false, error: String(error?.message || error || 'native view failed') };
  }
}
