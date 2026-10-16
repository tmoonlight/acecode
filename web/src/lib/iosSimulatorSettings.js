// 设置 > 工具 > iOS 模拟器 的视图模型(纯逻辑;组件在 components/IosSimulatorSettings.jsx)。
//
// 后端契约:GET /api/ios-simulator/status 给出启用状态、Xcode 发现结果、helper 与设备
// 授权;PUT /api/config/ios-simulator 接受任意字段子集并回传同形 status;
// POST /api/ios-simulator/xcode/inspect 实时校验用户输入的 Xcode 路径。

import { hasNativePreviewFilePicker, pickNativePreviewFile } from './desktopPreviewFilePicker.js';
import { requestPathPick } from './pathPickerHost.js';

export const IOS_SIMULATOR_AUTO_XCODE = 'auto';
export const IOS_SIMULATOR_CUSTOM_XCODE = 'custom';
export const IOS_SIMULATOR_FPS_OPTIONS = Object.freeze([15, 30, 60]);
export const IOS_SIMULATOR_SCALE_OPTIONS = Object.freeze([0.5, 0.75, 1]);

export function iosSimulatorQualityOptions() {
  return [
    { value: 0.6, label: '标准' },
    { value: 0.85, label: '高清' },
  ];
}

export function iosSimulatorScaleLabel(value) {
  return `${Math.round(Number(value) * 100)}%`;
}

// 设置项当前值不在候选里(手改了 config.json)时,下拉框显示最接近的一项。
export function nearestOption(value, options) {
  const list = (Array.isArray(options) ? options : []).map((option) => (
    typeof option === 'object' && option ? Number(option.value) : Number(option)));
  if (list.length === 0) return undefined;
  const target = Number(value);
  if (!Number.isFinite(target)) return list[0];
  return list.reduce((best, candidate) => (
    Math.abs(candidate - target) < Math.abs(best - target) ? candidate : best), list[0]);
}

// 去掉结尾斜杠与 /Contents/Developer,统一成 …/Xcode.app 口径再比较。
export function normalizeXcodeAppPath(path) {
  let value = String(path || '').trim().replace(/\/+$/, '');
  value = value.replace(/\/Contents\/Developer$/i, '');
  return value.replace(/\/+$/, '');
}

export function xcodeInstalls(status) {
  const installs = status?.xcode?.installs;
  return Array.isArray(installs) ? installs.filter((install) => install && install.app_path) : [];
}

export function sameXcodePath(left, right) {
  const a = normalizeXcodeAppPath(left);
  return !!a && a === normalizeXcodeAppPath(right);
}

// 单选的当前值:未配置 = 自动选择;配置路径命中某个已发现安装 = 该安装;否则为自定义。
export function selectedXcodeChoice(status) {
  const configured = String(status?.config?.xcode_path || '').trim();
  if (!configured) return IOS_SIMULATOR_AUTO_XCODE;
  const match = xcodeInstalls(status).find((install) => sameXcodePath(install.app_path, configured));
  return match ? match.app_path : IOS_SIMULATOR_CUSTOM_XCODE;
}

export function isSelectedXcodeInstall(status, install) {
  return !!status?.xcode?.selected && sameXcodePath(status.xcode.selected.app_path, install?.app_path);
}

export function xcodeSourceLabel(source) {
  switch (String(source || '')) {
    case 'configured': return '已配置';
    case 'DEVELOPER_DIR': return 'DEVELOPER_DIR';
    case 'xcode-select': return 'xcode-select';
    case 'applications': return '自动发现';
    case 'spotlight': return 'Spotlight';
    default: return String(source || '');
  }
}

export function xcodeSourceLabels(install) {
  const sources = Array.isArray(install?.sources) ? install.sources : [];
  return [...new Set(sources.map(xcodeSourceLabel).filter(Boolean))];
}

export function xcodeInstallTitle(install) {
  const version = String(install?.version || '').trim();
  const build = String(install?.build || '').trim();
  const head = version ? `Xcode ${version}` : 'Xcode';
  return build ? `${head} (${build})` : head;
}

// 页面顶部要提示的问题,按严重程度排列。kind: danger / warn / info。
export function iosSimulatorNotices(status) {
  if (!status) return [];
  if (status.supported === false) {
    return [{ key: 'unsupported', kind: 'info', text: 'iOS 模拟器仅在 macOS 上可用' }];
  }
  const notices = [];
  const xcode = status.xcode || {};
  if (xcode.configured_error) {
    notices.push({ key: 'configured', kind: 'danger', text: `所配置的 Xcode 不可用：${xcode.configured_error}` });
  }
  if (xcode.error) notices.push({ key: 'xcode', kind: 'danger', text: String(xcode.error) });
  if (!xcode.selected && !xcode.error && !xcode.configured_error) {
    notices.push({ key: 'missing', kind: 'warn', text: '未找到可用的 Xcode，请安装 Xcode 或在下方指定路径' });
  }
  if (xcode.selected && !String(xcode.core_simulator_version || '').trim()) {
    notices.push({
      key: 'core-simulator',
      kind: 'warn',
      text: '尚未安装模拟器组件：请先打开一次 Xcode，或运行 xcodebuild -runFirstLaunch',
    });
  }
  if (status.helper && status.helper.installed === false) {
    notices.push({ key: 'helper', kind: 'danger', text: '模拟器控制组件缺失，请重新安装完整的 ACECode 安装包' });
  }
  // The helper normally runs inside its Seatbelt profile; say so only when not.
  const sandboxNote = String(status.helper?.sandbox_note || '');
  if (status.helper?.running && status.helper.sandboxed === false && sandboxNote) {
    notices.push({ key: 'sandbox', kind: 'warn', text: `模拟器控制组件未在沙盒中运行：${sandboxNote}` });
  }
  return notices;
}

export function inspectResultView(result) {
  if (!result) return null;
  if (result.ok && result.install) {
    return {
      ok: true,
      text: `可用：${xcodeInstallTitle(result.install)}`,
      detail: String(result.install.developer_dir || result.install.app_path || ''),
    };
  }
  return { ok: false, text: String(result.message || '') || '该路径不是可用的 Xcode', detail: '' };
}

export function iosSimulatorErrorMessage(error, action = 'save') {
  const status = error?.status;
  if (error?.code === 'XCODE_INVALID') {
    const detail = String(error?.body?.message || '');
    return detail ? `该路径不是可用的 Xcode：${detail}` : '该路径不是可用的 Xcode';
  }
  if (status === 404 || status === 405) return '当前后台服务不支持 iOS 模拟器，请更新 ACECode 并重启后重试';
  if (status === 401 || status === 403) return '后台连接认证已失效，请重新连接 ACECode 后重试';
  const detail = String(error?.body?.message || '');
  if (action === 'load') return detail ? `加载 iOS 模拟器配置失败：${detail}` : '加载 iOS 模拟器配置失败';
  if (action === 'device') return detail || '模拟器操作失败，请重试';
  return detail ? `保存 iOS 模拟器配置失败：${detail}` : '保存 iOS 模拟器配置失败';
}

// 「浏览…」:Desktop 壳走原生文件对话框(macOS 把 .app 包当作文件,目录对话框选不中它);
// 其它页面走 web 路径选择器的目录模式(Xcode.app 在服务端目录列表里就是普通目录)。
export async function pickXcodePath({
  win = globalThis.window,
  webPicker = requestPathPick,
  initialPath = '/Applications',
} = {}) {
  if (hasNativePreviewFilePicker(win)) {
    const result = await pickNativePreviewFile(initialPath, win);
    return result.cancelled ? null : result.path;
  }
  const picked = await webPicker({ mode: 'folder', initialPath, purpose: 'settings' });
  return typeof picked?.path === 'string' && picked.path ? picked.path : null;
}
