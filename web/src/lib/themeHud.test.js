// 覆盖主题包的 HUD 外观(chrome)与多风格(variants):数据校验、风格解析、背景染色、
// 根节点属性、动效触发判断、外观偏好读写,以及 themeHud.css 只作用于 HUD 主题的约束。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  THEME_COLOR_KEYS, applyInstalledTheme, resolveThemeAppearance, resolveThemeVariant, selectThemeVariant,
  themeChromeStyle, themeCssProperties, themeVariantOptions, themeVariants, validThemeVariant,
} from './themePackages.js';
import { HUD_SWITCH_GRACE_MS, hudBurstForChange, hudVariantLabel } from './hudEffects.js';
import {
  appearancePreferencesToApi, mergeAppearancePreferences, normalizeAppearancePreferences,
  parseAppearancePreferences, validThemeVariantId,
} from './appearancePreferences.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8');
const palette = (hex) => Object.fromEntries(THEME_COLOR_KEYS.map((key) => [key, hex]));
const RED = { ...palette('#110000'), accent: '#FF4D57', 'send-bg': '#7DE7ED' };
const CYAN = { ...palette('#001111'), accent: '#2BD3F5', 'send-bg': '#FF5A6E' };
const YELLOW = { ...palette('#111100'), accent: '#F7E733', 'send-bg': '#2BD3F5' };
function hudTheme(extra = {}) {
  return {
    schema_version: 1, id: 'ai-hud-test', name: 'HUD test', version: '1.0.0', mode: 'dark',
    colors: RED,
    appearance: { logo_color: '#FF3B48', home_title_color: '#F6ECEA', extend_to_titlebar: true },
    chrome: { style: 'hud' },
    variants: [
      { id: 'red', name: '荒坂红', colors: RED },
      { id: 'cyan', name: '公共区青', colors: CYAN, tint: '#22CCF2', appearance: { logo_color: '#2BD3F5' } },
      { id: 'yellow', name: '限制区黄', colors: YELLOW, tint: '#F2E022' },
    ],
    ...extra,
  };
}
function fakeRoot() {
  const style = new Map();
  const attrs = new Map();
  return {
    style: { setProperty: (k, v) => style.set(k, v), removeProperty: (k) => style.delete(k) },
    setAttribute: (k, v) => attrs.set(k, String(v)),
    removeAttribute: (k) => attrs.delete(k),
    style$: style,
    attrs,
  };
}

// 场景:主题包声明 chrome。期望:只认识的 style(hud)生效;未知样式、错误类型一律忽略,
// 让以后新增的外观类型在这个版本上退化成普通主题,而不是整个主题判坏。
await run('chrome style is recognised only for known, well-formed values', () => {
  assert.equal(themeChromeStyle(hudTheme()), 'hud');
  for (const chrome of [undefined, null, 'hud', [], { style: 'glass' }, { style: 7 }, {}]) {
    assert.equal(themeChromeStyle(hudTheme({ chrome })), null, JSON.stringify(chrome));
  }
});

// 场景:variants 列表的校验是「全有或全无」。期望:合法列表原样返回;只要有一项坏掉
// (数量越界、重名、ID 格式、28 色不全、多余键、tint 非 #RRGGBB、外观参数非法、名字为空)
// 就整个忽略——部分保留会悄悄改变哪一项是默认风格。
await run('variant lists are accepted whole or ignored whole', () => {
  assert.equal(themeVariants(hudTheme()).length, 3);
  const base = hudTheme().variants;
  const broken = [
    [base[0]],
    [...base, ...base.map((v, i) => ({ ...v, id: `x${i}` })), { ...base[0], id: 'seventh' }],
    [base[0], { ...base[1], id: 'red' }],
    [base[0], { ...base[1], id: 'Cyan' }],
    [base[0], { ...base[1], id: 'cy--an' }],
    [base[0], { ...base[1], colors: { ...CYAN, accent: undefined } }],
    [base[0], { ...base[1], colors: { ...CYAN, extra: '#000000' } }],
    [base[0], { ...base[1], css: 'body{}' }],
    [base[0], { ...base[1], tint: 'red' }],
    [base[0], { ...base[1], appearance: { logo_color: 'url(x)' } }],
    [base[0], { ...base[1], name: '   ' }],
    'red,cyan',
  ];
  for (const variants of broken) assert.deepEqual(themeVariants(hudTheme({ variants })), [], JSON.stringify(variants).slice(0, 80));
  assert.equal(validThemeVariant(base[1]), true);
});

// 场景:按保存的风格 ID 选择。期望:空 ID、未知 ID(换了主题包或旧配置)都回到第一项;
// 没有 variants 的主题返回 null,渲染沿用顶层配色。
await run('variant selection falls back to the first entry', () => {
  assert.equal(selectThemeVariant(hudTheme(), 'cyan').id, 'cyan');
  assert.equal(selectThemeVariant(hudTheme(), '').id, 'red');
  assert.equal(selectThemeVariant(hudTheme(), 'missing').id, 'red');
  assert.equal(selectThemeVariant(hudTheme({ variants: undefined }), 'cyan'), null);
  assert.deepEqual(themeVariantOptions(hudTheme()).map((o) => [o.id, o.name, o.swatches]), [
    ['red', '荒坂红', ['#110000', '#FF4D57', '#7DE7ED']],
    ['cyan', '公共区青', ['#001111', '#2BD3F5', '#FF5A6E']],
    ['yellow', '限制区黄', ['#111100', '#F7E733', '#2BD3F5']],
  ]);
  assert.equal(hudVariantLabel(themeVariantOptions(hudTheme()), 'yellow'), '限制区黄');
  assert.equal(hudVariantLabel(null, 'yellow'), '');
});

// 场景:选中风格后用于渲染的定义。期望:28 色整体替换为该风格;外观参数逐键覆盖,
// 风格没写的键(标题色、通顶)保留顶层值;无 variants 的主题对象原样返回。
await run('resolved variants replace colors and overlay appearance keys', () => {
  const cyan = resolveThemeVariant(hudTheme(), 'cyan');
  assert.equal(cyan.colors.accent, '#2BD3F5');
  assert.deepEqual(cyan.appearance, { logo_color: '#2BD3F5', home_title_color: '#F6ECEA', extend_to_titlebar: true });
  assert.equal(resolveThemeAppearance(cyan).logoColor, '#2BD3F5');
  const plain = hudTheme({ variants: undefined });
  assert.equal(resolveThemeVariant(plain, 'cyan'), plain);
});

// 场景:风格带 tint 时背景图按「color」混合染色(保留图片明暗,换成 tint 的色相)。
// 期望:三张背景都插入同色渐变层并给出对应的 blend 列表;有透明度遮罩时遮罩仍在最上层;
// 用户消息图层尺寸同步多出一个 100% 100%。没有 tint 时输出与旧版本逐字一致。
await run('variant tint adds a color-blend layer without changing untinted output', () => {
  const definition = {
    ...hudTheme(),
    session_background: { bytes: 1, sha256: 'a'.repeat(64) },
    user_message_background: { bytes: 1, sha256: 'b'.repeat(64) },
    appearance: { ...hudTheme().appearance, user_message_background_opacity: 0.8 },
  };
  const urls = { sessionBackgroundUrl: 'blob:s', userMessageBackgroundUrl: 'blob:u' };
  const plain = themeCssProperties(definition, 'blob:h', urls);
  assert.deepEqual(themeCssProperties(definition, 'blob:h', urls, { tint: null }), plain);
  assert.equal(plain['--ace-home-background-blend'], undefined);
  const tinted = themeCssProperties(definition, 'blob:h', urls, { tint: '#22CCF2' });
  assert.equal(tinted['--ace-home-background-image'], 'linear-gradient(#22CCF2, #22CCF2), url("blob:h")');
  assert.equal(tinted['--ace-home-background-blend'], 'color, normal');
  assert.equal(tinted['--ace-session-background-blend'], 'color, normal');
  assert.match(tinted['--ace-user-message-background-image'], /^linear-gradient\(rgba\([^)]*\), rgba\([^)]*\)\), linear-gradient\(#22CCF2, #22CCF2\), url\("blob:u"\)$/);
  assert.equal(tinted['--ace-user-message-background-blend'], 'normal, color, normal');
  assert.equal(tinted['--ace-user-message-background-size'], '100% 100%, 100% 100%, 720px auto');
  // 非法 tint 不进入 CSS。
  assert.deepEqual(themeCssProperties(definition, 'blob:h', urls, { tint: 'red); x: (' }), plain);
});

// 场景:应用 HUD 主题。期望:根节点写入 data-theme-chrome / data-theme-variant /
// data-theme-effects,风格配色进入 CSS 变量;关闭动效时为 off;清理函数全部撤销。
// 普通主题(无 chrome、无 variants)不出现这三个属性,保证其它主题完全不受影响。
await run('applying a HUD theme writes chrome, variant and motion attributes', () => {
  const root = fakeRoot();
  const clear = applyInstalledTheme(root, hudTheme(), 'blob:h', {}, { variant: 'yellow', effects: false });
  assert.equal(root.attrs.get('data-theme-chrome'), 'hud');
  assert.equal(root.attrs.get('data-theme-variant'), 'yellow');
  assert.equal(root.attrs.get('data-theme-effects'), 'off');
  assert.equal(root.style$.get('--ace-accent'), '#F7E733');
  assert.equal(root.style$.get('--ace-home-background-blend'), 'color, normal');
  clear();
  assert.equal(root.attrs.size, 0);
  assert.equal(root.style$.size, 0);

  const plainRoot = fakeRoot();
  applyInstalledTheme(plainRoot, hudTheme({ chrome: undefined, variants: undefined }), 'blob:h', {}, { variant: 'cyan' });
  for (const attr of ['data-theme-chrome', 'data-theme-variant', 'data-theme-effects']) assert.equal(plainRoot.attrs.has(attr), false);
  assert.equal(plainRoot.style$.has('--ace-home-background-blend'), false);
});

// 场景:动效层的触发判断。期望:首次显示或换主题播放开屏扫描;同主题切换风格播放过场;
// 开屏后 1.5 秒内的风格变化是 daemon 恢复已保存的风格,不能再叠一段过场。
await run('motion bursts: boot on activation, switch on variant change after the grace period', () => {
  const red = { colorTheme: 'ai-a', variant: 'red' };
  assert.equal(hudBurstForChange(null, red), 'boot');
  assert.equal(hudBurstForChange(red, { colorTheme: 'ai-b', variant: 'red' }), 'boot');
  assert.equal(hudBurstForChange(red, { ...red, variant: 'cyan' }, HUD_SWITCH_GRACE_MS), 'switch');
  assert.equal(hudBurstForChange(red, { ...red, variant: 'cyan' }, HUD_SWITCH_GRACE_MS - 1), null);
  assert.equal(hudBurstForChange(red, { ...red }), null);
  assert.equal(hudBurstForChange(red, null), null);
});

// 场景:外观偏好读写风格与动效。期望:旧 daemon 不返回这两个键时按默认(默认风格、动效开),
// 不能让整份外观解析失败;只有显式 false 才关闭动效;非法风格 ID 归一为空串;
// 写回接口时带上 theme_variant / theme_effects。
await run('appearance preferences carry the theme variant and motion switch', () => {
  const legacy = parseAppearancePreferences({ theme: 'dark', color_theme: 'blue', font_size: 'medium' });
  assert.equal(legacy.themeVariant, '');
  assert.equal(legacy.themeEffects, true);
  const saved = parseAppearancePreferences({ theme: 'dark', color_theme: 'ai-x', font_size: 'small', theme_variant: 'cyan', theme_effects: false });
  assert.equal(saved.themeVariant, 'cyan');
  assert.equal(saved.themeEffects, false);
  assert.equal(normalizeAppearancePreferences({ theme_variant: 'Cyan' }).themeVariant, '');
  assert.equal(normalizeAppearancePreferences({ theme_effects: 'off' }).themeEffects, true);
  const merged = mergeAppearancePreferences(saved, { themeVariant: 'yellow' });
  assert.equal(merged.themeVariant, 'yellow');
  assert.equal(merged.themeEffects, false);
  const payload = appearancePreferencesToApi(merged);
  assert.equal(payload.theme_variant, 'yellow');
  assert.equal(payload.theme_effects, false);
  for (const id of ['', 'red', 'cyan-2']) assert.equal(validThemeVariantId(id), true);
  for (const id of ['Red', '-red', 'a--b', 'x'.repeat(25), 7, null]) assert.equal(validThemeVariantId(id), false);
});

// 场景:themeHud.css 的作用范围。期望:每条规则要么限定在 :root[data-theme-chrome="hud"] 下,
// 要么是只在 HUD 主题里挂载的动效层类名(.ace-hud-*);动画规则还必须要求动效开关为 on。
// 回归意义:这份样式随应用常驻加载,一条没加作用域的选择器就会改到所有主题。
await run('themeHud.css only styles HUD themes', () => {
  const css = read('../styles/themeHud.css').replace(/\/\*[\s\S]*?\*\//g, '');
  const blocks = [];
  let depth = 0; let start = 0; let atRule = [];
  for (let i = 0; i < css.length; i += 1) {
    if (css[i] === '{') {
      const prelude = css.slice(start, i).trim();
      if (prelude.startsWith('@')) atRule.push(depth); else if (!atRule.length) blocks.push(prelude);
      depth += 1; start = i + 1;
    } else if (css[i] === '}') {
      depth -= 1; start = i + 1;
      if (atRule.length && atRule[atRule.length - 1] === depth) atRule.pop();
    } else if (css[i] === ';' && !atRule.length && depth === 0) start = i + 1;
  }
  assert.ok(blocks.length > 40, `parsed ${blocks.length} rules`);
  const selectors = blocks.flatMap((prelude) => prelude.split(/,(?![^(]*\))/).map((s) => s.trim()).filter(Boolean));
  for (const selector of selectors) {
    const scoped = selector.startsWith(':root[data-theme-chrome="hud"]') || /^\.ace-hud-[a-z-]+/.test(selector);
    assert.ok(scoped, `unscoped selector: ${selector}`);
  }
  for (const prelude of blocks) {
    const body = css.slice(css.indexOf(prelude) + prelude.length, css.indexOf('}', css.indexOf(prelude)));
    if (/animation(-name)?\s*:/.test(body) && !prelude.includes('.ace-hud-')) {
      assert.match(prelude, /\[data-theme-effects="on"\]/, `animation without effects gate: ${prelude.slice(0, 80)}`);
    }
  }
});

// 场景:设置页的风格选择与动效开关。期望:只在当前主题有风格 / 声明 HUD 时渲染,
// 按钮带 data-theme-variant-option 便于定位;动效层由 ThemeProvider 挂载。
await run('settings and provider wire the variant picker and motion layer', () => {
  const settings = read('../components/SettingsPage.jsx');
  assert.match(settings, /themeVariantOptions\.length > 0 &&/);
  assert.match(settings, /data-theme-variant-option=\{option\.id\}/);
  assert.match(settings, /\{themeChrome && \(/);
  const provider = read('../theme.jsx');
  assert.match(provider, /<HudEffects[\s\S]*chrome=\{themeChrome\}[\s\S]*effects=\{themeEffects\}/);
});
