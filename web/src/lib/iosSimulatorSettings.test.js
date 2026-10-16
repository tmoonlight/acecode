import assert from 'node:assert/strict';
import { ApiError, createApi } from './api.js';
import {
  IOS_SIMULATOR_AUTO_XCODE,
  IOS_SIMULATOR_CUSTOM_XCODE,
  IOS_SIMULATOR_FPS_OPTIONS,
  inspectResultView,
  iosSimulatorErrorMessage,
  iosSimulatorNotices,
  iosSimulatorQualityOptions,
  iosSimulatorScaleLabel,
  isSelectedXcodeInstall,
  nearestOption,
  normalizeXcodeAppPath,
  pickXcodePath,
  sameXcodePath,
  selectedXcodeChoice,
  xcodeInstallTitle,
  xcodeInstalls,
  xcodeSourceLabel,
  xcodeSourceLabels,
} from './iosSimulatorSettings.js';
import { OPEN_SETTING_EVENT, requestOpenSetting } from './settingsNavigation.js';
import { findSettingsSearchEntry, settingsSearchEntries, searchSettings } from './settingsSearch.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const greenXcode = {
  app_path: '/Volumes/Tools/Xcode-26.app',
  developer_dir: '/Volumes/Tools/Xcode-26.app/Contents/Developer',
  version: '26.0',
  build: '17A324',
  simulator_kit: '/Volumes/Tools/Xcode-26.app/Contents/Developer/Library/PrivateFrameworks/SimulatorKit.framework',
  sources: ['spotlight', 'configured'],
};
const appXcode = {
  app_path: '/Applications/Xcode.app',
  developer_dir: '/Applications/Xcode.app/Contents/Developer',
  version: '16.4',
  build: '16F6',
  sources: ['applications', 'xcode-select'],
};

function statusFixture({ xcodePath = '', selected = appXcode, extra = {}, xcode = {} } = {}) {
  return {
    supported: true,
    enabled: true,
    config: { xcode_path: xcodePath, stream_fps: 30, stream_scale: 1, stream_quality: 0.6 },
    helper: { installed: true, running: false, developer_dir: '' },
    sessions: {},
    xcode: {
      installs: [appXcode, greenXcode],
      configured_path: xcodePath,
      configured_error: '',
      error: '',
      xcode_select_path: '/Applications/Xcode.app/Contents/Developer',
      core_simulator_version: '1010.2',
      selected_source: 'applications',
      selected,
      ...xcode,
    },
    device_consent: {},
    ...extra,
  };
}

await run('xcode selection maps the configured path onto auto, a detected install or custom', () => {
  assert.equal(selectedXcodeChoice(statusFixture()), IOS_SIMULATOR_AUTO_XCODE);
  assert.equal(selectedXcodeChoice(statusFixture({ xcodePath: '/Volumes/Tools/Xcode-26.app/' })), greenXcode.app_path);
  assert.equal(
    selectedXcodeChoice(statusFixture({ xcodePath: '/Volumes/Tools/Xcode-26.app/Contents/Developer' })),
    greenXcode.app_path,
    'a Contents/Developer path selects the same install',
  );
  assert.equal(selectedXcodeChoice(statusFixture({ xcodePath: '/Users/me/Xcode-beta.app' })), IOS_SIMULATOR_CUSTOM_XCODE);
  assert.equal(selectedXcodeChoice(null), IOS_SIMULATOR_AUTO_XCODE);
  assert.equal(normalizeXcodeAppPath(' /A/Xcode.app/Contents/Developer/ '), '/A/Xcode.app');
  assert.equal(sameXcodePath('/A/Xcode.app', '/A/Xcode.app/'), true);
  assert.equal(sameXcodePath('', ''), false);
  assert.equal(isSelectedXcodeInstall(statusFixture(), appXcode), true);
  assert.equal(isSelectedXcodeInstall(statusFixture(), greenXcode), false);
  assert.equal(isSelectedXcodeInstall(statusFixture({ selected: null }), appXcode), false);
  assert.deepEqual(xcodeInstalls({ xcode: { installs: [appXcode, null, { version: '1' }] } }), [appXcode]);
  assert.deepEqual(xcodeInstalls(null), []);
});

await run('xcode installs render a title and de-duplicated source tags', () => {
  assert.equal(xcodeInstallTitle(greenXcode), 'Xcode 26.0 (17A324)');
  assert.equal(xcodeInstallTitle({ version: '16.4' }), 'Xcode 16.4');
  assert.equal(xcodeInstallTitle({}), 'Xcode');
  assert.deepEqual(xcodeSourceLabels(greenXcode), ['Spotlight', '已配置']);
  assert.deepEqual(xcodeSourceLabels(appXcode), ['自动发现', 'xcode-select']);
  assert.deepEqual(xcodeSourceLabels({ sources: ['applications', 'applications', 'DEVELOPER_DIR'] }), ['自动发现', 'DEVELOPER_DIR']);
  assert.equal(xcodeSourceLabel('future-source'), 'future-source');
  assert.deepEqual(xcodeSourceLabels({}), []);
});

await run('status notices cover unsupported platforms, Xcode problems and missing components', () => {
  assert.deepEqual(iosSimulatorNotices(null), []);
  assert.deepEqual(iosSimulatorNotices({ supported: false }).map((notice) => [notice.key, notice.kind]), [['unsupported', 'info']]);
  assert.deepEqual(iosSimulatorNotices(statusFixture()), []);
  const broken = iosSimulatorNotices(statusFixture({
    xcodePath: '/bad',
    selected: null,
    xcode: { configured_error: 'not an Xcode bundle', error: '' },
  }));
  assert.deepEqual(broken.map((notice) => notice.key), ['configured']);
  assert.match(broken[0].text, /not an Xcode bundle/);
  assert.deepEqual(
    iosSimulatorNotices(statusFixture({ selected: null })).map((notice) => notice.key),
    ['missing'],
  );
  assert.deepEqual(
    iosSimulatorNotices(statusFixture({ selected: null, xcode: { error: 'xcrun failed' } })).map((notice) => [notice.key, notice.text]),
    [['xcode', 'xcrun failed']],
  );
  const firstLaunch = iosSimulatorNotices(statusFixture({ xcode: { core_simulator_version: '' } }));
  assert.deepEqual(firstLaunch.map((notice) => notice.key), ['core-simulator']);
  assert.match(firstLaunch[0].text, /xcodebuild -runFirstLaunch/);
  const helper = iosSimulatorNotices(statusFixture({ extra: { helper: { installed: false } } }));
  assert.deepEqual(helper.map((notice) => [notice.key, notice.kind]), [['helper', 'danger']]);
});

await run('inspect results and errors produce inline messages', () => {
  assert.equal(inspectResultView(null), null);
  assert.deepEqual(inspectResultView({ ok: true, install: greenXcode }), {
    ok: true,
    text: '可用：Xcode 26.0 (17A324)',
    detail: greenXcode.developer_dir,
  });
  assert.deepEqual(inspectResultView({ ok: false, message: 'missing Contents/Developer' }), {
    ok: false, text: 'missing Contents/Developer', detail: '',
  });
  assert.equal(inspectResultView({ ok: false }).text, '该路径不是可用的 Xcode');

  const invalid = new ApiError(400, { error: 'XCODE_INVALID', message: 'no xcodebuild' });
  assert.equal(iosSimulatorErrorMessage(invalid), '该路径不是可用的 Xcode：no xcodebuild');
  assert.match(iosSimulatorErrorMessage(new ApiError(404, 'Not Found')), /不支持 iOS 模拟器/);
  assert.match(iosSimulatorErrorMessage(new ApiError(401, {})), /认证已失效/);
  assert.equal(iosSimulatorErrorMessage(new ApiError(500, { error: 'PERSIST_FAILED', message: 'disk full' })),
    '保存 iOS 模拟器配置失败：disk full');
  assert.equal(iosSimulatorErrorMessage(new ApiError(500, {}), 'load'), '加载 iOS 模拟器配置失败');
  assert.equal(iosSimulatorErrorMessage(new ApiError(409, { error: 'XCODE_UNAVAILABLE', message: 'no simctl' }), 'device'), 'no simctl');
  assert.equal(iosSimulatorErrorMessage(new Error('boom'), 'device'), '模拟器操作失败，请重试');
});

await run('stream option helpers snap unknown values to the nearest choice', () => {
  assert.deepEqual(IOS_SIMULATOR_FPS_OPTIONS, [15, 30, 60]);
  assert.equal(nearestOption(24, IOS_SIMULATOR_FPS_OPTIONS), 30);
  assert.equal(nearestOption(5, IOS_SIMULATOR_FPS_OPTIONS), 15);
  assert.equal(nearestOption('bad', IOS_SIMULATOR_FPS_OPTIONS), 15);
  assert.equal(nearestOption(0.9, iosSimulatorQualityOptions()), 0.85);
  assert.equal(nearestOption(0.3, iosSimulatorQualityOptions()), 0.6);
  assert.equal(nearestOption(1, []), undefined);
  assert.deepEqual(iosSimulatorQualityOptions().map((option) => option.label), ['标准', '高清']);
  assert.equal(iosSimulatorScaleLabel(0.75), '75%');
});

await run('browse uses the native file dialog on Desktop and the web folder picker elsewhere', async () => {
  const nativeCalls = [];
  const desktop = {
    aceDesktop_pickPreviewFile: async (args) => {
      nativeCalls.push(args);
      return JSON.stringify({ ok: true, cancelled: false, path: '/Volumes/Tools/Xcode-26.app' });
    },
  };
  assert.equal(await pickXcodePath({ win: desktop, webPicker: () => assert.fail('no web picker on Desktop') }),
    '/Volumes/Tools/Xcode-26.app');
  assert.deepEqual(nativeCalls, [{ cwd: '/Applications' }]);
  const cancelled = { aceDesktop_pickPreviewFile: async () => ({ ok: true, cancelled: true }) };
  assert.equal(await pickXcodePath({ win: cancelled }), null);

  const webCalls = [];
  const picked = await pickXcodePath({
    win: {},
    webPicker: async (options) => {
      webCalls.push(options);
      return { path: '/Applications/Xcode.app', kind: 'directory' };
    },
  });
  assert.equal(picked, '/Applications/Xcode.app');
  assert.deepEqual(webCalls, [{ mode: 'folder', initialPath: '/Applications', purpose: 'settings' }]);
  assert.equal(await pickXcodePath({ win: {}, webPicker: async () => null }), null);
});

await run('the settings index and open-setting event find the iOS simulator card', () => {
  const entries = settingsSearchEntries(false);
  const entry = findSettingsSearchEntry(entries, { section: 'tools', label: 'iOS 模拟器' });
  assert.ok(entry, 'indexed under tools');
  assert.equal(entry.section, 'tools');
  for (const query of ['iOS', '模拟器', 'Simulator', 'Xcode']) {
    assert.ok(searchSettings(entries, query).some((item) => item.id === entry.id), query);
  }
  assert.equal(findSettingsSearchEntry(entries, { section: 'general', label: 'iOS 模拟器' }), null);
  assert.equal(findSettingsSearchEntry(entries, { label: '' }), null);

  const dispatched = [];
  const target = { dispatchEvent: (event) => dispatched.push(event) };
  assert.equal(requestOpenSetting({ section: 'tools', label: 'iOS 模拟器' }, target), true);
  assert.equal(dispatched[0].type, OPEN_SETTING_EVENT);
  assert.deepEqual(dispatched[0].detail, { section: 'tools', label: 'iOS 模拟器' });
  assert.equal(requestOpenSetting({}, null), false);
});

await run('iOS simulator API helpers use the documented endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options = {}) => {
    calls.push({ url, options });
    if (url.endsWith('/screenshot')) {
      return { ok: true, status: 200, headers: { get: () => 'image/png' }, blob: async () => ({ type: 'image/png' }) };
    }
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ devices: [] }) };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getIosSimulatorStatus();
    await client.getIosSimulatorStatus({ refresh: true });
    await client.setIosSimulatorConfig({ xcode_path: '', stream_fps: 60 });
    await client.inspectIosSimulatorXcode('/Volumes/Tools/Xcode.app');
    await client.listIosSimulatorDevices();
    await client.listIosSimulatorDevices('session 1');
    await client.iosSimulatorDeviceAction('UD/ID', 'boot');
    await client.iosSimulatorDeviceAction('UDID', 'consent', { allowed: true, name: 'iPhone' });
    const blob = await client.iosSimulatorScreenshot('UDID');
    assert.equal(blob.type, 'image/png');

    const base = 'http://127.0.0.1:4567';
    assert.deepEqual(calls.map((call) => [call.options.method, call.url]), [
      ['GET', `${base}/api/ios-simulator/status`],
      ['GET', `${base}/api/ios-simulator/status?refresh=1`],
      ['PUT', `${base}/api/config/ios-simulator`],
      ['POST', `${base}/api/ios-simulator/xcode/inspect`],
      ['GET', `${base}/api/ios-simulator/devices`],
      ['GET', `${base}/api/ios-simulator/devices?session=session%201`],
      ['POST', `${base}/api/ios-simulator/devices/UD%2FID/boot`],
      ['POST', `${base}/api/ios-simulator/devices/UDID/consent`],
      ['GET', `${base}/api/ios-simulator/devices/UDID/screenshot`],
    ]);
    assert.deepEqual(JSON.parse(calls[2].options.body), { xcode_path: '', stream_fps: 60 });
    assert.deepEqual(JSON.parse(calls[3].options.body), { path: '/Volumes/Tools/Xcode.app' });
    assert.deepEqual(JSON.parse(calls[7].options.body), { allowed: true, name: 'iPhone' });
    for (const call of calls) assert.equal(call.options.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});
