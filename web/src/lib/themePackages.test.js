import assert from 'node:assert/strict';
import {
  createThemeDownloadController, THEME_COLOR_KEYS, themeCssProperties,
  themeDownloadConsent, themeDownloadPercent, themeFailure, validThemeDefinition, releaseThemeResource,
  NATIONAL_DAY_THEME_ID,
} from './themePackages.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const entry = { id: 'eva-01', version: '1.0.0', installed: false, package: { bytes: 2000000, sha256: 'a'.repeat(64) } };
const pending = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };
const tick = () => new Promise((resolve) => setImmediate(resolve));

await run('workshop themes are discovered dynamically and use the existing verified install and apply flow', async () => {
  const remote = { ...entry, id: 'ai-dynamic-workshop', name: 'Dynamic theme', source: 'workshop' };
  const f = fixture(remote);
  await f.controller.refresh();
  assert.equal(f.controller.state().workshopEntries[0].id, remote.id);
  assert.deepEqual(f.controller.state().localEntries, []);
  await f.controller.install(remote);
  assert.equal(f.requests[0].id, remote.id);
  assert.equal(f.requests[0].consent.sha256, remote.package.sha256);
  f.complete(); await tick();
  assert.deepEqual(f.applies, [remote.id]);
  assert.equal(f.controller.state().workshopEntries[0].installed, true);
  assert.equal(f.controller.state().localEntries[0].id, remote.id);
  f.controller.dispose();
});

await run('newer workshop refresh wins and offline refresh preserves installed themes without stale offers', async () => {
  const first = pending(), second = pending();
  const remote = { ...entry, id: 'ai-workshop', name: 'Remote', source: 'workshop', installed: true, installed_version: '1.0.0' };
  let calls = 0;
  const controller = createThemeDownloadController({ api: { getThemes: () => ++calls === 1 ? first.promise : second.promise }, prepare() {}, apply() {} });
  const older = controller.refresh();
  controller.invalidateCatalog();
  const newer = controller.refresh();
  second.resolve({ themes: [remote] }); await newer;
  first.resolve({ themes: [{ ...remote, id: 'ai-stale' }] }); await older;
  assert.deepEqual(controller.state().workshopEntries.map((item) => item.id), ['ai-workshop']);
  controller.invalidateCatalog();
  assert.deepEqual(controller.state().workshopEntries, []);
  assert.equal(controller.state().localEntries[0].source, 'local');
  assert.equal(controller.state().localEntries[0].package, undefined);
  controller.dispose();
});

await run('withdrawn themes disappear while installed themes and workshop errors remain available', async () => {
  const remote = { ...entry, id: 'ai-workshop', name: 'Remote', source: 'workshop' };
  let catalog = { themes: [remote] };
  const controller = createThemeDownloadController({ api: { getThemes: async () => catalog }, prepare() {}, apply() {} });
  await controller.refresh();
  catalog = { themes: [] };
  await controller.refresh();
  assert.deepEqual(controller.state().workshopEntries, []);
  catalog = { themes: [{ ...remote, source: 'local', installed: true }], workshop_error: { error: 'THEME_WORKSHOP_UNAVAILABLE' } };
  await controller.refresh();
  assert.equal(controller.state().localEntries[0].id, remote.id);
  assert.match(controller.state().workshopError, /工坊/);
  controller.dispose();
});

await run('an old completed job cannot resurrect an uninstalled workshop theme', async () => {
  const remote = { ...entry, id: 'ai-workshop', name: 'Remote', source: 'workshop', installed: false };
  const controller = createThemeDownloadController({ api: { getThemes: async () => ({ themes: [remote],
    job: { id: remote.id, version: remote.version, state: 'completed' } }) }, prepare() {}, apply() {} });
  await controller.refresh();
  assert.equal(controller.state().workshopEntries[0].installed, false);
  assert.deepEqual(controller.state().localEntries, []);
  controller.dispose();
});

await run('an in-flight catalog cannot roll back a completed installation', async () => {
  const remote = { ...entry, id: 'ai-workshop', name: 'Remote', source: 'workshop' };
  const f = fixture(remote);
  await f.controller.refresh();
  const pendingCatalog = pending();
  f.api.getThemes = () => pendingCatalog.promise;
  const refreshing = f.controller.refresh();
  await f.controller.install(remote);
  f.complete(); await tick();
  pendingCatalog.resolve({ themes: [remote], job: { state: 'idle' } });
  await refreshing;
  assert.equal(f.controller.state().workshopEntries[0].installed, true);
  assert.deepEqual(f.applies, [remote.id]);
  f.controller.dispose();
});
function fixture(themeEntry = entry) {
  const completion = pending(), applies = [], applyOptions = [], prepared = [], preparations = [], requests = [];
  let currentJob = { id: themeEntry.id, version: themeEntry.version, state: 'downloading', bytes_total: 2000000, bytes_downloaded: 20 };
  let claims = 0;
  const api = {
    claimStartupTheme: async () => ({ id: NATIONAL_DAY_THEME_ID, claimed: claims++ === 0 }),
    getThemes: async () => ({ themes: [themeEntry], job: { state: 'idle' } }),
    installTheme: async (id, consent) => { requests.push({ id, consent }); return currentJob; },
    getThemeJob: async () => { await completion.promise; return currentJob; },
    cancelThemeInstall: async () => { currentJob = { id: themeEntry.id, state: 'cancelled' }; completion.resolve(); },
  };
  const controller = createThemeDownloadController({ api, prepare: async (id, options) => { prepared.push(id); preparations.push(options); }, apply: async (id, options) => { applies.push(id); applyOptions.push(options); }, wait: async () => {} });
  return { controller, api, applies, applyOptions, prepared, preparations, requests, complete: (state = 'completed') => {
    currentJob = { id: themeEntry.id, version: themeEntry.version, state, error: 'THEME_INVALID_PACKAGE' }; completion.resolve();
  } };
}

await run('startup automatically installs National Day once and does not mark EVA as installed', async () => {
  const f = fixture({ ...entry, id: NATIONAL_DAY_THEME_ID });
  const expectedAppearance = { theme: 'dark', color_theme: 'orange', font_size: 'large' };
  await Promise.all([f.controller.applyStartupTheme(expectedAppearance), f.controller.applyStartupTheme(expectedAppearance)]);
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0].consent.automatic, true);
  assert.deepEqual(f.applies, []);
  f.complete(); await tick();
  assert.deepEqual(f.applies, [NATIONAL_DAY_THEME_ID]);
  assert.deepEqual(f.applyOptions, [{ silent: true, expectedAppearance }]);
  assert.equal(f.controller.state().entries[0].installed, true);
  assert.equal(f.controller.state().entry, null);
  f.controller.dispose();
  const restarted = createThemeDownloadController({ api: f.api, prepare() {}, apply: () => assert.fail('startup repeated') });
  await restarted.applyStartupTheme();
  assert.equal(f.requests.length, 1);
  restarted.dispose();
});

await run('an installed National Day theme applies without a download or update prompt', async () => {
  const f = fixture({ ...entry, id: NATIONAL_DAY_THEME_ID, installed: true, update_available: true });
  await f.controller.applyStartupTheme();
  assert.deepEqual(f.requests, []);
  assert.deepEqual(f.prepared, [NATIONAL_DAY_THEME_ID]);
  assert.deepEqual(f.applies, [NATIONAL_DAY_THEME_ID]);
  f.controller.dispose();
});

await run('automatic claim, discovery, download, polling, validation and preparation failures are silent', async () => {
  for (const phase of ['claim', 'catalog', 'install', 'poll', 'validation', 'prepare', 'apply']) {
    const national = { ...entry, id: NATIONAL_DAY_THEME_ID };
    let applied = false;
    const fail = () => { throw new Error(phase); };
    const job = { id: national.id, version: national.version, automatic: true, state: 'downloading' };
    const api = {
      claimStartupTheme: async () => phase === 'claim' ? fail() : { id: national.id, claimed: true },
      getThemes: async () => phase === 'catalog' ? fail() : { themes: [national] },
      installTheme: async () => phase === 'install' ? fail() : job,
      getThemeJob: async () => phase === 'poll' ? fail() : { ...job, state: phase === 'validation' ? 'failed' : 'completed', error: 'THEME_INVALID_PACKAGE' },
    };
    const controller = createThemeDownloadController({ api, wait: async () => {},
      prepare: async () => { if (phase === 'prepare') fail(); },
      apply: async (_, options) => { assert.equal(options.silent, true); if (phase === 'apply') fail(); applied = true; },
    });
    await controller.applyStartupTheme(); await tick();
    assert.equal(applied, false, phase);
    assert.equal(controller.state().failure, null, phase);
    assert.equal(controller.state().error, '', phase);
    controller.dispose();
  }
});

await run('manual theme choices before discovery and during download supersede the startup theme', async () => {
  for (const duringClaim of [true, false]) {
    const f = fixture({ ...entry, id: NATIONAL_DAY_THEME_ID });
    const claim = pending();
    if (duringClaim) f.api.claimStartupTheme = () => claim.promise;
    const startup = f.controller.applyStartupTheme();
    if (!duringClaim) await startup;
    await f.controller.select('orange');
    claim.resolve({ id: NATIONAL_DAY_THEME_ID, claimed: true });
    await startup;
    f.complete(); await tick();
    assert.deepEqual(f.applies, ['orange']);
    if (duringClaim) assert.deepEqual(f.requests, []);
    f.controller.dispose();
  }
});

await run('automatic failure stays quiet in settings and a later manual retry reports errors', async () => {
  const f = fixture({ ...entry, id: NATIONAL_DAY_THEME_ID });
  await f.controller.applyStartupTheme();
  f.complete('failed'); await tick();
  f.api.getThemes = async () => { throw new Error('offline'); };
  await f.controller.refresh();
  assert.equal(f.controller.state().failure, null);
  assert.equal(f.controller.state().error, '');
  await f.controller.refresh({ notify: true, id: NATIONAL_DAY_THEME_ID });
  assert.match(f.controller.state().failure.message, /offline/);
  f.controller.dispose();
});

await run('theme palette only accepts registered hex tokens and safe application blob URLs', () => {
  const definition = { schema_version: 1, id: 'eva-01', version: '1.0.0', mode: 'light', colors: Object.fromEntries(THEME_COLOR_KEYS.map((key) => [key, '#ABCDEF'])) };
  assert.equal(validThemeDefinition(definition), true);
  const css = themeCssProperties(definition, 'blob:http://localhost/123');
  assert.equal(css['--ace-bg-rgb'], '171, 205, 239');
  assert.equal(css['--ace-home-background-image'], 'url("blob:http://localhost/123")');
  assert.equal(themeCssProperties(definition, 'https://example.com/image.png')['--ace-home-background-image'], undefined);
  assert.equal(validThemeDefinition({ ...definition, colors: { ...definition.colors, bg: 'url(javascript:alert(1))' } }), false);
  assert.equal(validThemeDefinition({ ...definition, mode: 'dark' }), false);
});

await run('theme consent pins the displayed size, version and checksum', () => {
  assert.deepEqual(themeDownloadConsent(entry), { confirm_download: true, bytes: 2000000, version: '1.0.0', sha256: 'a'.repeat(64) });
  assert.throws(() => themeDownloadConsent({ ...entry, package: { bytes: 0 } }));
  assert.equal(themeDownloadPercent({ bytes_total: 200, bytes_downloaded: 50 }), 25);
  assert.equal(themeDownloadPercent({ bytes_total: 200, bytes_downloaded: 250 }), 100);
});

await run('catalog previews do not download a theme and only completed installs apply it', async () => {
  for (const id of ['eva-01', NATIONAL_DAY_THEME_ID]) {
    const themeEntry = { ...entry, id };
    const f = fixture(themeEntry);
    await f.controller.refresh();
    assert.deepEqual(f.requests, []);
    assert.deepEqual(f.applies, []);
    await f.controller.install(themeEntry);
    assert.deepEqual(f.requests, [{ id, consent: themeDownloadConsent(themeEntry) }]);
    assert.deepEqual(f.applies, []);
    f.complete(); await tick();
    assert.deepEqual(f.prepared, [id]);
    assert.deepEqual(f.applies, [id]);
    assert.equal(f.controller.state().entries.find((item) => item.id === id).installed, true);
    f.controller.dispose();
  }
});

await run('a later manual theme choice wins over an in-flight installation', async () => {
  const f = fixture();
  await f.controller.install(entry);
  await f.controller.select('orange');
  f.complete(); await tick();
  assert.deepEqual(f.applies, ['orange']);
  assert.deepEqual(f.prepared, []);
  f.controller.dispose();
});

await run('cancelled and failed installs keep the current appearance', async () => {
  const cancelled = fixture();
  await cancelled.controller.install(entry);
  await cancelled.controller.cancel(); await tick();
  assert.deepEqual(cancelled.applies, []);
  assert.equal(cancelled.controller.state().job.state, 'cancelled');
  cancelled.controller.dispose();
  const failed = fixture();
  await failed.controller.install(entry);
  failed.complete('failed'); await tick();
  assert.deepEqual(failed.applies, []);
  assert.match(failed.controller.state().error, /校验失败/);
  failed.controller.dispose();
});

await run('changing theme during asset loading prevents stale automatic application', async () => {
  const asset = pending(), applies = [];
  const controller = createThemeDownloadController({ api: {}, prepare: () => asset.promise, apply: (id) => applies.push(id) });
  const selecting = controller.select('eva-01');
  await controller.select('blue');
  asset.resolve(); await selecting;
  assert.deepEqual(applies, ['blue']);
  controller.dispose();
});

await run('reopening settings can observe completion without dropping automatic application', async () => {
  const f = fixture();
  await f.controller.install(entry);
  f.api.getThemes = async () => ({ themes: [{ ...entry, installed: true }], job: { id: 'eva-01', version: entry.version, state: 'completed' } });
  await f.controller.refresh();
  f.complete(); await tick();
  assert.deepEqual(f.applies, ['eva-01']);
  f.controller.dispose();
});

await run('passive catalogue failures stay quiet while a user retry reports the actual address', async () => {
  const f = fixture();
  const path = 'https://mirror.example/custom/themes/catalog.json';
  f.api.getThemes = async () => { throw { code: 'THEME_CATALOG_UNAVAILABLE', body: { error_path: path } }; };
  assert.equal(await f.controller.refresh(), null);
  assert.equal(f.controller.state().failure, null);
  assert.deepEqual(f.requests, []);
  assert.equal(await f.controller.refresh({ notify: true }), null);
  assert.equal(f.controller.state().failure.path, path);
  f.controller.dismissFailure();
  await f.controller.refresh();
  assert.equal(f.controller.state().failure, null);
  f.controller.dispose();
});

await run('download failure remains available to the shell and dismissing it does not reopen a stale error', async () => {
  const f = fixture();
  const path = 'https://mirror.example/custom/themes/eva-01/1.0.0/theme.zip';
  const failedJob = { state: 'failed', error: 'THEME_DOWNLOAD_FAILED', error_path: path };
  f.api.getThemeJob = async () => failedJob;
  await f.controller.install(entry);
  await tick();
  assert.equal(f.controller.state().failure.path, path);
  assert.deepEqual(f.applies, []);
  f.controller.dismissFailure();
  f.api.getThemes = async () => ({ themes: [entry], job: failedJob });
  await f.controller.refresh();
  assert.equal(f.controller.state().failure, null);
  f.controller.dispose();
});

await run('failure paths prefer backend detail and preserve local file paths', () => {
  assert.equal(themeFailure({ code: 'THEME_SAVE_FAILED', body: { error_path: 'C:\\Themes\\EVA\\theme.json' } }, 'https://mirror.example/theme.zip').path, 'C:\\Themes\\EVA\\theme.json');
  assert.equal(themeFailure(new Error('Network disconnected'), 'https://mirror.example/theme.zip').path, 'https://mirror.example/theme.zip');
});

await run('an installed theme update waits for consent and reloads cached resources on completion', async () => {
  const update = { ...entry, version: '1.0.1', installed: true, installed_version: '1.0.0', update_available: true };
  const f = fixture(update);
  await f.controller.refresh();
  assert.deepEqual(f.requests, []);
  await f.controller.install(update);
  assert.equal(f.requests[0].consent.version, '1.0.1');
  f.complete(); await tick();
  assert.deepEqual(f.preparations, [{ refresh: true }]);
  assert.deepEqual(f.applies, ['eva-01']);
  assert.equal(f.controller.state().entry.installed_version, '1.0.1');
  assert.equal(f.controller.state().entry.update_available, false);
  f.controller.dispose();
});

await run('replayed older completion neither hides an update nor prematurely applies it', async () => {
  const update = { ...entry, version: '1.0.1', installed: true, installed_version: '1.0.0', update_available: true };
  const f = fixture(update);
  f.api.getThemes = async () => ({ themes: [update], job: { id: 'eva-01', version: '1.0.0', state: 'completed' } });
  await f.controller.refresh();
  assert.equal(f.controller.state().entry.update_available, true);
  await f.controller.install(update);
  await f.controller.refresh();
  assert.deepEqual(f.prepared, []);
  assert.equal(f.controller.state().entry.update_available, true);
  f.complete(); await tick();
  assert.deepEqual(f.applies, ['eva-01']);
  assert.equal(f.controller.state().entry.update_available, false);
  f.controller.dispose();
});

await run('a failed update retains the installed version and does not replace the loaded theme', async () => {
  const update = { ...entry, version: '1.0.1', installed: true, installed_version: '1.0.0', update_available: true };
  const f = fixture(update);
  await f.controller.refresh();
  await f.controller.install(update);
  f.complete('failed'); await tick();
  assert.deepEqual(f.prepared, []);
  assert.deepEqual(f.applies, []);
  assert.equal(f.controller.state().entry.installed_version, '1.0.0');
  assert.equal(f.controller.state().entry.update_available, true);
  f.controller.dispose();
});

await run('AI theme definitions validate all tokens in either fixed mode', () => {
  const definition = { schema_version: 1, id: 'ai-night', name: '夜色', version: '1.0.0', mode: 'dark', colors: Object.fromEntries(THEME_COLOR_KEYS.map((key) => [key, '#123ABC'])) };
  assert.equal(validThemeDefinition(definition), true);
  assert.equal(validThemeDefinition({ ...definition, mode: 'light' }), true);
  assert.equal(validThemeDefinition({ ...definition, mode: 'system' }), false);
  assert.equal(validThemeDefinition({ ...definition, name: ' ' }), false);
  assert.equal(validThemeDefinition({ ...definition, id: 'ai-../blue' }), false);
  assert.equal(validThemeDefinition({ ...definition, colors: { ...definition.colors, 'extra-css': '#123ABC' } }), false);
  const css = themeCssProperties(definition, 'blob:http://localhost/night');
  assert.equal(css['--ace-bg-rgb'], '18, 58, 188');
  assert.equal(css['--ace-send-bg'], '#123ABC');
  assert.equal(css['--ace-home-title-color'], '#123ABC');
  assert.equal(Object.keys(css).length, THEME_COLOR_KEYS.length * 2 + 2);
});

await run('offline catalog keeps local themes selectable without a remote package', async () => {
  const f = fixture();
  const local = { id: 'ai-night', name: '夜色', source: 'local', installed: true, version: '1.0.0' };
  f.api.getThemes = async () => ({ themes: [{ id: 'eva-01', available: false, installed: false }, local], offline: true,
    catalog_error: { error: 'THEME_CATALOG_UNAVAILABLE' } });
  await f.controller.refresh();
  assert.deepEqual(f.controller.state().localEntries, [local]);
  assert.match(f.controller.state().error, /主题信息/);
  await f.controller.select(local.id);
  assert.deepEqual(f.prepared, ['ai-night']);
  assert.deepEqual(f.applies, ['ai-night']);
  assert.deepEqual(f.requests, []);
  f.api.getThemes = async () => ({ themes: [local] });
  await f.controller.refresh();
  assert.deepEqual(f.controller.state().localEntries, [local]);
  f.controller.dispose();
});

await run('created local theme refreshes, prepares and uses the normal appearance application', async () => {
  const f = fixture();
  await f.controller.created({ id: 'ai-night', version: '1.0.0', apply: true }, f.controller.beginCreation());
  assert.deepEqual(f.prepared, ['ai-night']);
  assert.deepEqual(f.preparations, [{ refresh: true }]);
  assert.deepEqual(f.applies, ['ai-night']);
  f.controller.dispose();
});

await run('later selection wins while a created theme is preparing', async () => {
  const waiting = pending(), applies = [];
  const controller = createThemeDownloadController({ api: { getThemes: async () => ({ themes: [] }) },
    prepare: () => waiting.promise, apply: (id) => applies.push(id) });
  const creating = controller.created({ id: 'ai-night', apply: true }, controller.beginCreation());
  await tick();
  await controller.select('orange');
  waiting.resolve();
  await creating;
  assert.deepEqual(applies, ['orange']);
  controller.dispose();
});

await run('created-theme resource errors retain the current appearance and report the actual failure', async () => {
  const applies = [];
  const controller = createThemeDownloadController({ api: { getThemes: async () => ({ themes: [] }) },
    prepare: async () => { throw new Error('PNG checksum mismatch'); }, apply: (id) => applies.push(id) });
  await controller.created({ id: 'ai-night', apply: true }, controller.beginCreation());
  assert.deepEqual(applies, []);
  assert.equal(controller.state().failure.message, 'PNG checksum mismatch');
  controller.dispose();
});

await run('local deletion removes its card and image without waiting for a remote catalogue', async () => {
  const local = { id: 'ai-night', name: '夜色', source: 'local', installed: true, version: '1.0.0' };
  const refreshing = pending(), forgotten = [];
  let count = 0;
  const controller = createThemeDownloadController({ api: { getThemes: () => ++count === 1 ? Promise.resolve({ themes: [local] }) : refreshing.promise },
    prepare: async () => {}, apply: async () => {}, forget: (id) => forgotten.push(id), remove: async (id) => ({ id, deleted: true, cleanup_pending: true }) });
  await controller.refresh();
  const removed = await controller.remove(local.id);
  assert.equal(removed.cleanup_pending, true);
  assert.deepEqual(controller.state().localEntries, []);
  assert.deepEqual(forgotten, [local.id]);
  assert.equal(controller.state().deletingId, '');
  refreshing.resolve({ themes: [local] });
  await tick();
  assert.deepEqual(controller.state().localEntries, [], 'a stale catalogue must not restore a removed card');
  controller.dispose();
});

await run('failed deletion keeps local cards and cached resources while builtin removal is rejected', async () => {
  const local = { id: 'ai-night', name: '夜色', source: 'local', installed: true };
  const controller = createThemeDownloadController({ api: { getThemes: async () => ({ themes: [local] }) }, prepare: async () => {}, apply: async () => {},
    forget: () => assert.fail('must retain resources'), remove: async () => { throw new Error('read only'); } });
  await controller.refresh();
  await assert.rejects(controller.remove(local.id), /read only/);
  assert.deepEqual(controller.state().localEntries, [local]);
  await assert.rejects(controller.remove('blue'));
  assert.equal(controller.state().deletingId, '');
  controller.dispose();
});

await run('deleted resources cannot be applied by a late selection or creation completion', async () => {
  const asset = pending(), applies = [], released = [];
  const controller = createThemeDownloadController({ api: { getThemes: async () => ({ themes: [] }) }, prepare: () => asset.promise,
    apply: (id) => applies.push(id), remove: async (id) => ({ id, deleted: true }), forget: (id) => released.push(id) });
  const selecting = controller.select('ai-night');
  await controller.remove('ai-night');
  asset.resolve();
  await selecting;
  await controller.created({ id: 'ai-night', apply: true }, controller.beginCreation());
  assert.deepEqual(applies, []);
  assert.deepEqual(released, ['ai-night']);
  await controller.select('orange');
  assert.deepEqual(applies, ['orange']);
  controller.dispose();
});

await run('evicted pending image releases its eventual Blob while other theme resources remain cached', async () => {
  const image = pending(), released = [];
  const other = Promise.resolve({ backgroundUrl: 'blob:other' });
  const cache = new Map([['ai-night', image.promise], ['ai-other', other]]);
  releaseThemeResource(cache, 'ai-night', (url) => released.push(url));
  assert.equal(cache.has('ai-night'), false);
  assert.equal(cache.get('ai-other'), other);
  image.resolve({ backgroundUrl: 'blob:removed' });
  await tick();
  assert.deepEqual(released, ['blob:removed']);
});
