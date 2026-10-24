import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { clsx } from './format.js';
import * as packages from './themePackages.js';
import * as exports from './themeExports.js';
import { isInstalledColorTheme } from './colorTheme.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const source = readFileSync(new URL('../components/ThemeCards.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const body = ast.program.body.filter((node) => node.type !== 'ImportDeclaration').map((node) => node.declaration || node);
const component = body.find((node) => node.id?.name === 'ThemeCards');
const stateNames = component.body.body.flatMap((node) => node.declarations || [])
  .filter((node) => node.init?.callee?.name === 'useState').map((node) => node.id.elements[0].name);
const transformed = await transformWithEsbuild(body.map((node) => source.slice(node.start, node.end)).join('\n'), 'ThemeCards.jsx', { loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment' });
function components(states = {}) {
  let stateIndex = 0;
  return vm.runInNewContext(`${transformed.code}; ({ ThemeCards, LocalThemeCard });`, {
    React, clsx, ...packages, ...exports, api: {},
    useEffect() {}, useMemo: (factory) => factory(),
    useState(initial) { const name = stateNames[stateIndex++]; return [Object.hasOwn(states, name) ? states[name] : initial, () => {}]; },
    Modal: ({ children, layerClassName, labelledBy }) => React.createElement('section', { role: 'dialog', 'data-layer': layerClassName, 'aria-labelledby': labelledBy }, children),
    VsIcon: ({ name, size, className }) => React.createElement('span', { 'data-icon-name': name, 'aria-hidden': true, className, style: { width: size, height: size } }),
    toast() {},
  });
}
const local = { id: 'ai-eva', name: 'EVA 初号机 · 自定义', source: 'local', installed: true, version: '1.0.0', swatches: ['#ABCDEF', '#FFFFFF', '#123456'] };
const options = [{ key: 'blue', label: '蓝色' }, { key: 'orange', label: '橙色' }];
const props = { options, selected: local.id, onSelect() {}, onCreateAiTheme() {}, downloads: { localEntries: [local], entry: { installed: true } } };
await run('dynamic workshop card renders before installation and installed IDs appear once', () => {
  const { ThemeCards } = components();
  const remote = { ...local, source: 'workshop', installed: false, key: 'a'.repeat(64), name: '工坊动态主题' };
  const html = renderToStaticMarkup(React.createElement(ThemeCards, { ...props, selected: 'blue',
    downloads: { localEntries: [local], workshopEntries: [remote], job: { state: 'idle' } } }));
  assert.equal((html.match(/data-theme-id="ai-eva"/g) || []).length, 1);
  assert.match(html, /工坊动态主题/);
  assert.match(html, /未安装/);
  assert.doesNotMatch(html, /导出主题|删除主题/);
  const installed = renderToStaticMarkup(React.createElement(ThemeCards, { ...props,
    downloads: { localEntries: [local], workshopEntries: [{ ...remote, installed: true, update_available: true }], job: { state: 'idle' } } }));
  assert.equal((installed.match(/data-theme-id="ai-eva"/g) || []).length, 1);
  assert.match(installed, /可更新/);
  assert.match(installed, /导出主题/);
});
function descendants(element) {
  if (!React.isValidElement(element)) return [];
  return [element, ...React.Children.toArray(element.props.children).flatMap(descendants)];
}

await run('built-in National Day precedes EVA with an offline preview and isolated progress', () => {
  const { ThemeCards } = components();
  const html = renderToStaticMarkup(React.createElement(ThemeCards, {
    ...props, downloads: { entries: [], job: { id: packages.NATIONAL_DAY_THEME_ID, state: 'downloading', bytes_total: 100, bytes_downloaded: 25 } },
  }));
  const national = html.indexOf('data-theme-id="national-day-2026"');
  const eva = html.indexOf('data-theme-id="eva-01"');
  assert.ok(national >= 0 && eva > national);
  assert.match(html.slice(national, eva), /national-day-2026-thumbnail\.png/);
  assert.match(html.slice(national, eva), /role="progressbar"/);
  assert.doesNotMatch(html.slice(eva), /role="progressbar"/);
});

await run('production theme cards render only two custom management links with no nested button or archive icon', () => {
  const { ThemeCards } = components();
  const html = renderToStaticMarkup(React.createElement(ThemeCards, props));
  assert.equal((html.match(/>导出主题<\/button>/g) || []).length, 1);
  assert.equal((html.match(/>删除主题<\/button>/g) || []).length, 1);
  assert.match(html, /使用中/);
  assert.match(html, /AI主题/);
  const { LocalThemeCard } = components();
  const tree = LocalThemeCard({ entry: local, selected: true, onSelect() {}, onExport() {}, onDelete() {} });
  const buttons = descendants(tree).filter((node) => node.type === 'button');
  assert.equal(buttons.length, 3);
  for (const button of buttons) assert.equal(descendants(button).filter((node) => node.type === 'button').length, 1);
  assert.equal(descendants(tree).some((node) => node.type === 'svg'), false);
  for (const id of ['blue', 'orange', 'eva-01', 'national-day-2026']) {
    const html = renderToStaticMarkup(React.createElement(LocalThemeCard, { entry: { ...local, id }, onSelect() {} }));
    assert.doesNotMatch(html, /导出主题|删除主题/);
  }
});

await run('production card action callbacks stop propagation and never apply a theme', () => {
  const calls = [], { LocalThemeCard } = components();
  const tree = LocalThemeCard({ entry: local, selected: false, onSelect: () => calls.push('select'), onExport: (entry) => calls.push(`export:${entry.id}`), onDelete: (entry) => calls.push(`delete:${entry.id}`) });
  const buttons = descendants(tree).filter((node) => node.type === 'button');
  for (const button of buttons.slice(1)) button.props.onClick({ stopPropagation: () => calls.push('stop') });
  assert.deepEqual(calls, ['stop', 'export:ai-eva', 'stop', 'delete:ai-eva']);
});

await run('production compression modal displays measured progress without images or SVGs and unknown progress has no value', () => {
  for (const progress of [null, 0.68]) {
    const { ThemeCards } = components({ exportState: { ...exports.EMPTY_THEME_EXPORT, status: 'compressing', entry: local, filename: '主题.zip', hadCompression: true, job: { progress } } });
    const html = renderToStaticMarkup(React.createElement(ThemeCards, props));
    const modal = html.slice(html.indexOf('<section'));
    assert.match(modal, /data-layer="z-\[400\]"/);
    assert.match(modal, /正在打包主题/);
    assert.match(modal, /<progress/);
    assert.doesNotMatch(modal, /<img|<svg/);
    if (progress === null) assert.doesNotMatch(modal, /value=|[0-9]+%/);
    else { assert.match(modal, /value="0.68"/); assert.match(modal, /68/); }
  }
  const { ThemeCards } = components({ exportState: { ...exports.EMPTY_THEME_EXPORT, status: 'saving', entry: local, job: { reused: true } } });
  assert.doesNotMatch(renderToStaticMarkup(React.createElement(ThemeCards, props)), /<section|<progress/);
});

await run('production deletion confirmation is above Settings and accurately describes current-theme fallback', () => {
  const { ThemeCards } = components({ deleteTarget: local });
  const html = renderToStaticMarkup(React.createElement(ThemeCards, props));
  const modal = html.slice(html.indexOf('<section'));
  assert.match(modal, /data-layer="z-\[400\]"/);
  assert.match(modal, /删除后将切换为蓝色主题/);
  assert.match(modal, /EVA 初号机 · 自定义/);
  assert.doesNotMatch(modal, /<img|<svg/);
  const css = readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
  assert.match(css, /:hover, :focus-within, \.has-active-operation/);
  assert.match(css, /@media \(hover: none\)\s*\{\s*\.ace-local-theme \.ace-theme-card-actions \{ opacity: 1; pointer-events: auto; \}/);
  assert.doesNotMatch(css, /\.ace-theme-card-state\s*\{\s*opacity: 0/);
});

await run('real ThemeProvider callbacks discard late image results and release both old and failed replacement resources', async () => {
  const text = readFileSync(new URL('../theme.jsx', import.meta.url), 'utf8');
  const parsed = parseSync(text, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const provider = parsed.program.body.map((node) => node.declaration || node).find((node) => node.id?.name === 'ThemeProvider');
  const callback = (name) => {
    const node = provider.body.body.flatMap((item) => item.declarations || []).find((item) => item.id.name === name).init.arguments[0];
    return text.slice(node.start, node.end);
  };
  const themeCache = { current: new Map() }, themeReloadRequired = { current: new Set() }, revoked = [];
  let installed = {}, rejectImage;
  const definition = { ...local, schema_version: 1, mode: 'light', colors: Object.fromEntries(packages.THEME_COLOR_KEYS.map((key) => [key, '#ABCDEF'])) };
  const api = { getTheme: async () => definition, readThemeImage: async () => 'old' };
  const context = { themeCache, themeReloadRequired, mounted: { current: true }, api,
    isInstalledColorTheme, validThemeDefinition: packages.validThemeDefinition, releaseThemeResource: (cache, id) => packages.releaseThemeResource(cache, id, (url) => revoked.push(url)),
    loadThemeResources: (theme, readImage) => packages.loadThemeResources(theme, readImage, (value) => `blob:${value}`, (url) => revoked.push(url)),
    revokeThemeResources: (item) => packages.revokeThemeResources(item, (url) => revoked.push(url)),
    URL: { createObjectURL: (value) => `blob:${value}`, revokeObjectURL: (url) => revoked.push(url) },
    setInstalledThemes(update) { installed = update(installed); } };
  const prepare = vm.runInNewContext(`(${callback('prepareTheme')})`, context);
  const forget = vm.runInNewContext(`(${callback('forgetTheme')})`, context);
  await prepare(local.id);
  assert.equal(installed[local.id].backgroundUrl, 'blob:old');
  api.readThemeImage = () => new Promise((resolve, reject) => { rejectImage = reject; });
  const refreshing = prepare(local.id, { refresh: true });
  await new Promise((resolve) => setImmediate(resolve));
  forget(local.id);
  rejectImage(new Error('removed'));
  await assert.rejects(refreshing, /removed/);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(themeCache.current.size, 0);
  assert.equal(Object.keys(installed).length, 0);
  assert.ok(revoked.includes('blob:old'));
  api.readThemeImage = async () => 'late';
  const loading = prepare(local.id);
  forget(local.id);
  await loading;
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(installed[local.id], undefined);
  assert.ok(revoked.includes('blob:late'));
});
