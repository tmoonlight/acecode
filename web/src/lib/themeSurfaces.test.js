import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { DOMParser } from '@xmldom/xmldom';
import { transformWithEsbuild } from 'vite';
import {
  applyInstalledTheme, EVA_THEME_ID, THEME_COLOR_KEYS, resolveThemeAppearance,
  themeCssProperties, validThemeAppearance, validThemeDefinition, loadThemeResources, revokeThemeResources,
} from './themePackages.js';
import { themeLogoDataUrl, themeLogoPalette, themeLogoRgb } from './brandLogoColors.js';
import * as logoPerformance from './interactiveHomeLogoPerformance.js';
import { HOME_LOGO_SHADER_ENABLED } from './homeLogoEffectPolicy.js';
import { renderMarkdownBlocks } from './markdown.js';
import { useStreamingRenderThrottle } from './useStreamingRenderThrottle.js';
import { assistantChromeState } from './assistantAvatarDisplay.js';
import { presentSystemNotice } from './systemNotice.js';
import { tr } from '../i18n/index.js';
import { clsx } from './format.js';
import { userMessageTextPreview } from './userMessagePreview.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8');
const logoSvg = read('../../../assets/branding/acecode-icon.svg');
function definition(appearance, mode = 'dark', id = 'ai-surface-test') {
  return {
    schema_version: 1, id, name: 'Surface test', version: '1.0.0', mode,
    colors: Object.fromEntries(THEME_COLOR_KEYS.map((key) => [key, '#123456'])),
    ...(appearance === undefined ? {} : { appearance }),
  };
}

await run('theme appearance accepts absent and partial overrides without changing 28-color packages', () => {
  for (const appearance of [undefined, {}, { logo_color: '#aB12Ef' }, { home_title_color: '#010203' },
    { extend_to_titlebar: false }, { logo_color: '#000000', home_title_color: '#FFFFFF', extend_to_titlebar: true }]) {
    assert.equal(validThemeDefinition(definition(appearance)), true);
  }
  assert.equal(THEME_COLOR_KEYS.length, 28);
  assert.equal(validThemeDefinition({ ...definition({}), colors: { ...definition({}).colors, other: '#112233' } }), false);
  for (const appearance of [null, undefined, [], true, 'purple', 1, { unknown: true },
    { logo_color: null }, { logo_color: '#FFF' }, { logo_color: '#123456\n' },
    { logo_color: 'url(javascript:alert(1))' }, { home_title_color: '#12345g' },
    { home_title_color: '#123456; color: red' }, { extend_to_titlebar: null },
    { extend_to_titlebar: 1 }, { extend_to_titlebar: 'true' }]) {
    assert.equal(validThemeAppearance(appearance), false, JSON.stringify(appearance));
    assert.equal(validThemeDefinition({ ...definition(), appearance }), false);
    assert.throws(() => themeCssProperties({ ...definition(), appearance }));
  }
});

await run('theme appearance preserves legacy defaults and lets explicit titlebar choices win', () => {
  assert.deepEqual(resolveThemeAppearance(definition()), {
    logoColor: null, homeTitleColor: '#123456', extendToTitlebar: false, whiteTitlebarControls: false,
  });
  assert.deepEqual(resolveThemeAppearance(definition({}, 'light', EVA_THEME_ID)), {
    logoColor: null, homeTitleColor: '#123456', extendToTitlebar: true, whiteTitlebarControls: true,
  });
  for (const mode of ['light', 'dark']) {
    for (const extend of [false, true]) {
      const appearance = resolveThemeAppearance(definition({ extend_to_titlebar: extend }, mode));
      assert.equal(appearance.extendToTitlebar, extend);
      assert.equal(appearance.whiteTitlebarControls, extend && mode === 'dark');
    }
  }
  for (const extend of [false, true]) {
    const appearance = resolveThemeAppearance(definition({ extend_to_titlebar: extend }, 'light', EVA_THEME_ID));
    assert.equal(appearance.extendToTitlebar, extend);
    assert.equal(appearance.whiteTitlebarControls, false);
  }
  const full = definition({ logo_color: '#FE0123', home_title_color: '#ABCDEF', extend_to_titlebar: true });
  const css = themeCssProperties(full, 'blob:http://localhost/theme');
  assert.equal(css['--ace-logo-color'], '#FE0123');
  assert.equal(css['--ace-logo-color-rgb'], '254, 1, 35');
  assert.equal(css['--ace-home-title-color'], '#ABCDEF');
  assert.equal(css['--ace-fg'], '#123456');
});

await run('background opacity is bounded and new fields preserve existing logo, title and titlebar behavior', () => {
  const original = { logo_color: '#FE0123', home_title_color: '#ABCDEF', extend_to_titlebar: true };
  const expanded = { ...original, home_composer_opacity: 0.7, home_background_opacity: 0,
    home_background_color: '#102030', session_background_opacity: 0.5, user_message_background_opacity: 1 };
  for (const value of [0, 0.3, 1]) assert.equal(validThemeAppearance({ home_composer_opacity: value }), true);
  for (const value of [-0.1, 1.1, NaN, Infinity, '0.7', true, null]) {
    for (const key of ['home_composer_opacity', 'home_background_opacity', 'session_background_opacity', 'user_message_background_opacity']) {
      assert.equal(validThemeAppearance({ [key]: value }), false);
    }
  }
  for (const mode of ['light', 'dark']) for (const extend of [false, true]) {
    assert.deepEqual(resolveThemeAppearance(definition({ ...expanded, extend_to_titlebar: extend }, mode)),
      resolveThemeAppearance(definition({ ...original, extend_to_titlebar: extend }, mode)));
  }
  const image = { bytes: 100, sha256: 'a'.repeat(64) };
  const custom = { ...definition(expanded), session_background: image, user_message_background: image };
  const css = themeCssProperties(custom, 'blob:home', { sessionBackgroundUrl: 'blob:session', userMessageBackgroundUrl: 'blob:user' });
  assert.equal(css['--ace-home-composer-opacity'], '0.7');
  assert.match(css['--ace-home-background-image'], /rgba\(16, 32, 48, 1\)/);
  assert.match(css['--ace-session-background-image'], /0\.5\).*blob:session/);
  assert.match(css['--ace-user-message-background-image'], /blob:user/);
  assert.equal(css['--ace-logo-color'], original.logo_color);
  assert.equal(css['--ace-home-title-color'], original.home_title_color);
  const legacy = themeCssProperties(definition(original), 'blob:home', { sessionBackgroundUrl: 'blob:ignored' });
  assert.equal(legacy['--ace-home-background-image'], 'url("blob:home")');
  assert.equal(legacy['--ace-session-background-image'], undefined);
  assert.equal(legacy['--ace-home-composer-opacity'], undefined);
  assert.equal(themeCssProperties(custom, 'blob:home', { sessionBackgroundUrl: 'https://invalid.test/image' })['--ace-session-background-image'], undefined);
  assert.equal(validThemeDefinition({ ...custom, session_background: { bytes: 0, sha256: 'a'.repeat(64) } }), false);
});

await run('all background URLs are released on success, failure and theme cleanup', async () => {
  const image = { bytes: 100, sha256: 'a'.repeat(64) };
  const custom = { ...definition({ extend_to_titlebar: true }), session_background: image, user_message_background: image };
  const requests = [], revoked = [];
  const loaded = await loadThemeResources(custom, async (kind) => { requests.push(kind); return kind; }, (kind) => 'blob:' + kind);
  assert.deepEqual(requests, ['background', 'session-background', 'user-message-background']);
  revokeThemeResources(loaded, (url) => revoked.push(url));
  assert.equal(new Set(revoked).size, 3);
  const styles = new Map(), attributes = new Map();
  const root = { style: { setProperty: (key, value) => styles.set(key, value), removeProperty: (key) => styles.delete(key) },
    setAttribute: (key, value) => attributes.set(key, value), removeAttribute: (key) => attributes.delete(key) };
  const clear = applyInstalledTheme(root, loaded, loaded.backgroundUrl, loaded);
  assert.equal(attributes.get('data-theme-titlebar-controls'), 'white');
  assert.equal(attributes.get('data-theme-session-background'), 'true');
  assert.equal(attributes.get('data-theme-user-message-background'), 'true');
  clear(); assert.equal(styles.size, 0); assert.equal(attributes.size, 0);
  const release = applyInstalledTheme(root, definition(), 'blob:old');
  assert.equal(attributes.has('data-theme-session-background'), false);
  assert.equal(attributes.has('data-theme-user-message-background'), false);
  release();
  const failedRevokes = [];
  await assert.rejects(loadThemeResources(custom, async (kind) => {
    if (kind === 'session-background') throw new Error('missing session');
    await new Promise((resolve) => setTimeout(resolve, 1));
    return kind;
  }, (kind) => 'blob:' + kind, (url) => failedRevokes.push(url)), /missing session/);
  assert.deepEqual(failedRevokes.sort(), ['blob:background', 'blob:user-message-background']);
});

await run('user wallpaper uses one artwork scale across message widths while its veil fills the bubble', () => {
  const image = { bytes: 100, sha256: 'a'.repeat(64) };
  for (const opacity of [undefined, 0, 0.7, 1]) {
    const appearance = { user_message_background_color: '#FFD078' };
    if (opacity !== undefined) appearance.user_message_background_opacity = opacity;
    const custom = { ...definition(appearance), user_message_background: image };
    const properties = themeCssProperties(custom, 'blob:home', { userMessageBackgroundUrl: 'blob:user' });
    assert.equal(properties['--ace-user-message-background-color'], '#FFD078');
    assert.equal(properties['--ace-user-message-background-size'],
      opacity === undefined ? '720px auto' : '100% 100%, 720px auto');
    const missing = themeCssProperties(custom, 'blob:home');
    assert.equal(missing['--ace-user-message-background-size'], undefined);
  }
  const styles = read('../styles/globals.css');
  const rule = styles.match(/\[data-theme-user-message-background="true"\] \.ace-user-message-bubble \{([^}]+)\}/)[1];
  assert.match(rule, /background-position: right bottom/);
  assert.match(rule, /background-size: var\(--ace-user-message-background-size, 720px auto\)/);
  assert.match(rule, /background-repeat: no-repeat/);
  assert.doesNotMatch(rule, /opacity:/);
});

await run('installed theme application clears all appearance state on switching, removal and missing wallpaper', () => {
  const styles = new Map(), attributes = new Map([['data-theme', 'dark']]);
  const root = {
    style: { setProperty: (key, value) => styles.set(key, value), removeProperty: (key) => styles.delete(key) },
    setAttribute: (key, value) => attributes.set(key, value), removeAttribute: (key) => attributes.delete(key),
  };
  const custom = definition({ logo_color: '#009944', home_title_color: '#F0F0F0', extend_to_titlebar: true });
  const clear = applyInstalledTheme(root, custom, 'blob:http://localhost/theme');
  assert.equal(attributes.get('data-theme-wallpaper'), 'true');
  assert.equal(attributes.get('data-theme-extend-to-titlebar'), 'true');
  assert.equal(attributes.get('data-theme-titlebar-controls'), 'white');
  assert.equal(attributes.get('data-theme-logo-color'), 'custom');
  clear();
  assert.equal(styles.size, 0);
  assert.deepEqual([...attributes], [['data-theme', 'dark']]);
  const clearLegacy = applyInstalledTheme(root, definition(), 'blob:http://localhost/old');
  assert.equal(attributes.get('data-theme-extend-to-titlebar'), 'false');
  assert.equal(attributes.has('data-theme-titlebar-controls'), false);
  assert.equal(styles.has('--ace-logo-color'), false);
  assert.equal(styles.get('--ace-home-title-color'), '#123456');
  clearLegacy();
  for (const url of [undefined, 'https://example.com/wallpaper.png', 'blob:bad url']) {
    const release = applyInstalledTheme(root, custom, url);
    for (const key of ['data-theme-wallpaper', 'data-theme-extend-to-titlebar', 'data-theme-titlebar-controls']) {
      assert.equal(attributes.has(key), false);
    }
    release();
  }
  applyInstalledTheme(root, null)();
  assert.deepEqual([...attributes], [['data-theme', 'dark']]);
  assert.equal(styles.size, 0);
});

function shapes(svg) {
  const document = new DOMParser().parseFromString(svg, 'image/svg+xml');
  return ['path', 'rect'].flatMap((tag) => Array.from(document.getElementsByTagName(tag)).map((node) => (
    ['d', 'x', 'y', 'width', 'height', 'rx', 'stroke-width', 'stroke-linecap', 'stroke-linejoin', 'fill-rule']
      .map((key) => [key, node.getAttribute(key)])
  )));
}
await run('custom brand colors preserve exact bundled geometry and white glyphs across RGB extremes', () => {
  for (const color of ['#A14BFF', '#F03020', '#000000', '#FFFFFF', '#808080', '#7f8081']) {
    const palette = themeLogoPalette(color);
    assert.equal(palette.base, color);
    assert.ok(Object.values(palette).every((value) => /^#[0-9a-f]{6}$/i.test(value)));
    assert.ok(themeLogoRgb(color).every((value) => Number.isFinite(value) && value >= 0 && value <= 1));
    const svg = decodeURIComponent(themeLogoDataUrl(logoSvg, color).split(',')[1]);
    assert.deepEqual(shapes(svg), shapes(logoSvg));
    assert.ok(svg.includes(`<stop offset="0.7" stop-color="${color}"/>`));
    assert.ok(svg.includes('<stop stop-color="#FFFFFF"/>'));
    assert.ok(svg.includes('<stop offset="1" stop-color="#F1F1F1"/>'));
    assert.ok(svg.includes('<stop offset="1" stop-color="#ECECEC"/>'));
    assert.doesNotMatch(svg, /NaN|Infinity/);
  }
  for (const invalid of [undefined, '', '#fff', 'red', '#123456\n', '#123456"/><script>']) {
    assert.throws(() => themeLogoPalette(invalid), TypeError);
  }
});

async function compiledComponent(relative, name) {
  const source = read(relative);
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const body = ast.program.body.filter((node) => node.type !== 'ImportDeclaration').map((node) => node.declaration || node);
  const transformed = await transformWithEsbuild(body.map((node) => source.slice(node.start, node.end)).join('\n'), relative, {
    loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
  });
  return (globals = {}) => vm.runInNewContext(`${transformed.code}; ${name};`, {
    React, ...React, logoSvg, themeLogoDataUrl, themeLogoRgb, EVA_THEME_ID, HOME_LOGO_SHADER_ENABLED, ...logoPerformance, ...globals,
  });
}
const brandComponent = await compiledComponent('../components/BrandLogo.jsx', 'BrandLogo');
const homeComponent = await compiledComponent('../components/InteractiveHomeLogo.jsx', 'InteractiveHomeLogo');
const messageComponent = await compiledComponent('../components/Message.jsx', 'Message');
const activityLineComponent = await compiledComponent('../components/ActivityLine.jsx', 'ActivityLine');
await run('actual messages restrict custom wallpaper to user text and preserve assistant, attachment and system surfaces', () => {
  const Message = messageComponent({
    useTranslation: () => ({ t: tr }), useSlashCommands: () => ({ commands: [] }),
    resolveLeadingSlashCommand: () => null, renderMarkdownBlocks, useStreamingRenderThrottle, assistantChromeState,
    presentSystemNotice, clsx, VsIcon: () => null, userMessageTextPreview,
    ActivityLine: activityLineComponent({ clsx, VsIcon: () => null }),
    AttachmentStrip: ({ align }) => React.createElement('span', { 'data-attachment-align': align }, 'Attachment'),
  });
  const render = (role, content) => renderToStaticMarkup(React.createElement(Message, { role, content, showFooter: false }));
  const user = new DOMParser().parseFromString(render('user', 'User text'), 'text/html');
  const bubbles = Array.from(user.getElementsByTagName('div')).filter((node) => node.getAttribute('class')?.split(' ').includes('ace-user-message-bubble'));
  assert.equal(bubbles.length, 1);
  assert.equal(bubbles[0].textContent, 'User text');
  assert.equal(bubbles[0].getElementsByTagName('span').length, 0, 'Attachments remain outside the wallpaper bubble');
  assert.doesNotMatch(render('user', ''), /ace-user-message-bubble/);
  for (const role of ['assistant', 'system', 'error', 'tool_result']) {
    const markup = render(role, 'A visible message');
    if (role === 'system' || role === 'tool_result') {
      assert.match(markup, /data-unified-activity-line="true"/);
      assert.match(markup, /aria-expanded="false"/);
      assert.doesNotMatch(markup, /data-code-copy-source/);
    } else {
      assert.match(markup, /A visible message/);
    }
    assert.doesNotMatch(markup, /ace-user-message-bubble/);
  }
});
let activeTheme;
const useTheme = () => activeTheme;
const BrandLogo = brandComponent({ useTheme });
const InteractiveHomeLogo = homeComponent({ useTheme, BrandLogo });

await run('actual home and sidebar logo components share custom SVG color and preserve fallback policies', () => {
  for (const color of [null, '#D42C37']) {
    activeTheme = { colorTheme: 'ai-surface-test', appearance: { logoColor: color } };
    const sidebar = renderToStaticMarkup(React.createElement(BrandLogo, { width: 20, height: 20, className: 'ace-brand-logo' }));
    const home = renderToStaticMarkup(React.createElement(InteractiveHomeLogo, { enabled: true }));
    const fallback = renderToStaticMarkup(React.createElement(InteractiveHomeLogo, { enabled: false }));
    assert.doesNotMatch(home, /<canvas/);
    assert.match(home, /data-dynamic-logo-ready="false"/);
    assert.doesNotMatch(fallback, /<canvas/);
    assert.match(home, /data-dynamic-logo-fallback="disabled"/);
    assert.match(fallback, /data-dynamic-logo-fallback="disabled"/);
    const source = (markup) => markup.match(/src="([^"]+)"/)[1];
    assert.equal(source(sidebar), source(home));
    assert.equal(source(home), source(fallback));
    assert.equal(source(home).startsWith('data:image/svg+xml;'), !!color);
    if (!color) assert.equal(source(home), '/acecode-logo.png');
  }
  for (const color of [null, '#D42C37']) {
    activeTheme = { colorTheme: EVA_THEME_ID, appearance: { logoColor: color } };
    const home = renderToStaticMarkup(React.createElement(InteractiveHomeLogo, { enabled: true }));
    assert.doesNotMatch(home, /<canvas/);
    assert.match(home, /data-dynamic-logo-fallback="disabled"/);
  }
});

function rendererHarness({ color, reducedMotion = false, noGl = false, shaderEnabled = HOME_LOGO_SHADER_ENABLED }) {
  const effects = [], frames = new Map(), timers = new Map(), events = new Map(), uniforms = new Map();
  let nextId = 0, drawCount = 0, contextRequests = 0;
  const listen = (target) => ({
    addEventListener: (name, fn) => events.set(`${target}:${name}`, fn),
    removeEventListener: (name) => events.delete(`${target}:${name}`),
  });
  const gl = new Proxy({
    getShaderParameter: () => true, getProgramParameter: () => true, isContextLost: () => false,
    getUniformLocation: (_, name) => name,
    uniform1f: (name, value) => uniforms.set(name, value),
    uniform3f: (name, ...value) => uniforms.set(name, value),
    drawArrays: () => { drawCount += 1; },
  }, { get: (object, key) => object[key] || (/^[A-Z_]+$/.test(key) ? 1 : () => ({})) });
  const canvas = {
    width: 156, height: 156, ...listen('canvas'),
    getContext: () => { contextRequests += 1; return noGl ? null : gl; },
    getBoundingClientRect: () => ({ width: 156, height: 156, left: 0, top: 0, bottom: 156 }),
  };
  const observer = class { observe() {} disconnect() {} };
  const globals = {
    HOME_LOGO_SHADER_ENABLED: shaderEnabled,
    useTheme: () => ({ colorTheme: 'ai-surface-test', appearance: { logoColor: color } }), BrandLogo,
    useRef: () => ({ current: canvas }), useState: (value) => [value, () => {}],
    useEffect: (fn) => effects.push(fn), MutationObserver: observer, ResizeObserver: observer,
    document: { hidden: false, documentElement: { dataset: { theme: 'dark' } }, ...listen('document') },
    window: {
      ...listen('window'), devicePixelRatio: 1,
      matchMedia: () => ({ matches: reducedMotion, ...listen('media') }),
      requestAnimationFrame: (fn) => { const id = ++nextId; frames.set(id, fn); return id; },
      cancelAnimationFrame: (id) => frames.delete(id),
      setTimeout: (fn) => { const id = ++nextId; timers.set(id, fn); return id; },
      clearTimeout: (id) => timers.delete(id),
    },
  };
  const Component = homeComponent(globals);
  Component({ enabled: true });
  const cleanup = effects[0]();
  const frame = () => {
    const [id, draw] = frames.entries().next().value;
    frames.delete(id); draw(2000);
  };
  return { uniforms, events, timers, frames, cleanup, frame, drawCount: () => drawCount, contextRequests: () => contextRequests };
}

await run('home shader stays suspended while the retained renderer preserves pointer, idle and reduced-motion paths', () => {
  const disabled = rendererHarness({ color: '#9B6DFF' });
  assert.equal(disabled.contextRequests(), 0);
  assert.equal(disabled.frames.size, 0);
  assert.equal(disabled.timers.size, 0);
  assert.equal(disabled.events.size, 0);
  assert.equal(disabled.drawCount(), 0);
  assert.equal(disabled.cleanup, undefined);

  // Enable only the isolated harness to keep coverage of the retained shader code.
  for (const color of ['#000000', '#FFFFFF', '#7f8081', '#E03145', null]) {
    const renderer = rendererHarness({ color, shaderEnabled: true });
    renderer.frame();
    assert.deepEqual([...renderer.uniforms.get('u_logo_color')], color ? themeLogoRgb(color) : [0, 0, 0]);
    assert.equal(renderer.uniforms.get('u_custom_logo'), color ? 1 : 0);
    renderer.events.get('window:pointermove')({ clientX: 100, clientY: 100 });
    renderer.frame();
    assert.equal(renderer.uniforms.get('u_pointer_active'), 1);
    renderer.timers.values().next().value();
    renderer.frame();
    assert.equal(renderer.uniforms.get('u_custom_logo'), color ? 1 : 0);
    assert.ok(renderer.drawCount() >= 3);
    renderer.events.get('canvas:webglcontextlost')({ preventDefault() {} });
    assert.equal(renderer.frames.size, 0);
    renderer.cleanup();
    assert.equal(renderer.events.size, 0);
  }
  const reduced = rendererHarness({ color: '#9B6DFF', reducedMotion: true, shaderEnabled: true });
  reduced.frame();
  assert.equal(reduced.timers.size, 0);
  assert.equal(reduced.uniforms.get('u_custom_logo'), 1);
  reduced.cleanup();
  const fallback = rendererHarness({ color: '#9B6DFF', noGl: true, shaderEnabled: true });
  assert.equal(fallback.frames.size, 0);
  assert.equal(fallback.cleanup, undefined);
});

await run('home title and titlebar CSS scope overrides while preserving native close feedback and sidebar chrome', () => {
  const styles = read('../styles/globals.css');
  const provider = read('../theme.jsx');
  const sidebar = read('../components/Sidebar.jsx');
  const chat = read('../components/ChatView.jsx');
  assert.match(styles, /\.ace-home-title\s*\{[^}]*color: var\(--ace-home-title-color, var\(--ace-fg\)\)/);
  assert.match(chat, /\? '我们该做什么？'\s*:\s*`我们该在 \$\{homeProjectName\} 中做什么？`/);
  assert.match(chat, /<h1 className="ace-home-title">\{homeProjectTitle\}<\/h1>/);
  assert.match(sidebar, /<BrandLogo width="20" height="20" className="ace-brand-logo block shrink-0"/);
  assert.match(provider, /return clearInstalledTheme;/);
  assert.match(styles, /:root\[data-color-theme="eva-01"\]:not\(\[data-theme-logo-color="custom"\]\) \.ace-brand-logo/);
  assert.match(styles, /\[data-theme-extend-to-titlebar="false"\] \.ace-app-shell\[data-home-wallpaper="true"\]::before \{ top: var\(--ace-topbar-height\); \}/);
  assert.match(styles, /\[data-theme-extend-to-titlebar="false"\][^\n]*\.ace-topbar \{ background-color: rgba\(var\(--ace-surface-rgb\), \.95\); \}/);
  assert.match(styles, /\[data-theme-titlebar-controls="white"\] \.ace-app-shell\[data-home-wallpaper="true"\] \.ace-topbar-controls :is\(\.ace-topbar-action, \.ace-window-control\) \{\s*color: white;/);
  assert.match(styles, /\.ace-window-control:not\(\.ace-window-control-close\)\):is\(:hover, :focus-visible\)/);
  assert.match(styles, /\.ace-window-control-close:focus-visible \{\s*background: #e81123;\s*color: #fff;/);
  assert.match(styles, /inset: 0 0 0 var\(--ace-home-sidebar-width, 0px\);/);
  assert.match(styles, /\.ace-topbar::before \{[^}]*background: var\(--ace-shell-bg\);/);
});
