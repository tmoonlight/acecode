import assert from 'node:assert/strict';
import { themeWorkshopUrl, validateThemeImportFile, observeThemeWorkshopUrl, UPGRADE_CONFIG_CHANGED_EVENT } from './themeImports.js';
import { createThemeDownloadController } from './themePackages.js';

for (const [source, expected] of [
  ['http://2017studio.imwork.net:82/aupdate/', 'http://2017studio.imwork.net:10082/'],
  ['http://192.168.1.2:1234/aupdate/', 'http://192.168.1.2:11234/'],
  ['http://updates.example/custom/path?x=1#part', 'http://updates.example:10080/'],
  ['https://updates.example/custom/path?x=1#part', 'https://updates.example:10443/'],
  ['http://updates.example:80/', 'http://updates.example:10080/'],
  ['https://updates.example:443/', 'https://updates.example:10443/'],
  ['http://[::1]:1234/path', 'http://[::1]:11234/'],
  ['http://localhost:55535/', 'http://localhost:65535/'],
  ['  http://localhost:82/  ', 'http://localhost:10082/'],
]) assert.equal(themeWorkshopUrl(source), expected);
for (const source of ['', 'bad-url', 'http://localhost:0/', 'http://localhost:55536/', 'http://localhost:65535/', 'http://localhost:65536/']) {
  assert.throws(() => themeWorkshopUrl(source));
}
assert.throws(() => themeWorkshopUrl('javascript:alert(1)'));
assert.throws(() => themeWorkshopUrl('https://secret:token@updates.example/aupdate'));
assert.throws(() => validateThemeImportFile({ name: 'picture.png', size: 12 }));
assert.throws(() => validateThemeImportFile({ name: 'theme.zip', size: 16 * 1024 * 1024 + 1 }));
console.log('[pass] workshop URL derives port +10000 and rejects invalid addresses');

{
  const target = new EventTarget(), pending = [], urls = [], errors = [];
  const tick = () => new Promise((resolve) => setImmediate(resolve));
  const dispose = observeThemeWorkshopUrl({
    target, loadConfig: () => new Promise((resolve, reject) => pending.push({ resolve, reject })),
    onUrl: (url) => urls.push(url), onError: (error) => errors.push(error),
  });
  await tick();
  target.dispatchEvent(new Event(UPGRADE_CONFIG_CHANGED_EVENT));
  await tick();
  pending[1].resolve({ base_url: 'http://host:1234/aupdate/' });
  await tick();
  pending[0].resolve({ base_url: 'http://old:82/aupdate/' });
  await tick();
  assert.equal(urls.at(-1), 'http://host:11234/');
  target.dispatchEvent(new Event('focus'));
  await tick();
  assert.equal(urls.at(-1), '');
  pending[2].resolve({ base_url: 'http://host:60000/' });
  await tick();
  assert.match(errors.at(-1), /主题工坊地址无效/);
  target.dispatchEvent(new Event('focus'));
  await tick();
  dispose();
  const count = urls.length;
  pending[3].resolve({ base_url: 'http://late:82/' });
  target.dispatchEvent(new Event(UPGRADE_CONFIG_CHANGED_EVENT));
  await tick();
  assert.equal(urls.length, count);
  assert.equal(pending.length, 4);
}
console.log('[pass] workshop entry refresh ignores stale requests and disposed observers');

for (const scenario of ['apply', 'keep-current', 'later-selection']) {
  let complete;
  const applied = [], prepared = [], forgotten = [];
  const controller = createThemeDownloadController({
    api: { importTheme: () => new Promise((resolve) => { complete = resolve; }), getThemes: async () => ({ themes: [], job: { state: 'idle' } }) },
    prepare: async (id, options) => prepared.push([id, options]), apply: async (id) => applied.push(id), forget: (id) => forgotten.push(id),
  });
  const pending = controller.importLocal({}, 'a'.repeat(64), scenario !== 'keep-current');
  if (scenario === 'later-selection') await controller.select('orange');
  complete({ id: 'ai-imported', version: '1.0.0' });
  const result = await pending;
  assert.deepEqual(forgotten, ['ai-imported']);
  assert.deepEqual(applied, scenario === 'apply' ? ['ai-imported'] : scenario === 'later-selection' ? ['orange'] : []);
  assert.equal(result.applied, scenario === 'apply');
  if (scenario === 'apply') assert.deepEqual(prepared, [['ai-imported', { refresh: true }]]);
  controller.dispose();
}
console.log('[pass] local import honors apply choice, refreshes resources, and preserves later theme selections');
