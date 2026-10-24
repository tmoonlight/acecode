// 主题音效:配方完整性、主题包声明、静音门控、节流、点击目标判定、交互监听与接线。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { THEME_SOUND_STYLES, themeSoundStyle } from './themePackages.js';
import {
  THEME_SOUND_NAMES, THEME_SOUND_RECIPES, configureThemeSounds, createThemeSoundThrottle,
  installThemeSoundInteractions, playThemeSound, resetThemeSoundsForTest, scheduleThemeSound,
  themeSoundDuration, themeSoundForTarget, themeSoundsEnabled,
} from './themeSounds.js';

async function run(name, fn) { await fn(); console.log(`[pass] ${name}`); }
const read = (relative) => readFileSync(new URL(relative, import.meta.url), 'utf8');

// 记录调度结果的假 AudioContext:只实现音效引擎用到的节点与参数方法。
function fakeAudioScope() {
  const log = { sources: [], gains: [], filters: [] };
  const param = () => {
    const events = [];
    return {
      value: 0, events,
      setValueAtTime: (v, t) => events.push(['set', v, t]),
      linearRampToValueAtTime: (v, t) => events.push(['linear', v, t]),
      exponentialRampToValueAtTime: (v, t) => events.push(['exp', v, t]),
    };
  };
  const node = (extra = {}) => ({ connect() {}, disconnect() {}, ...extra });
  class FakeContext {
    constructor() { this.state = 'running'; this.currentTime = 1; this.sampleRate = 8000; this.destination = node(); }
    createGain() { const gain = node({ gain: param() }); log.gains.push(gain); return gain; }
    createBiquadFilter() { const filter = node({ type: '', frequency: param(), Q: param() }); log.filters.push(filter); return filter; }
    createDynamicsCompressor() { return node({ threshold: param(), knee: param(), ratio: param(), attack: param(), release: param() }); }
    createBuffer(channels, length) { const data = new Float32Array(length); return { getChannelData: () => data }; }
    createOscillator() { return this.source('tone', { frequency: param() }); }
    createBufferSource() { return this.source('noise', {}); }
    source(kind, extra) {
      const source = node({ kind, ...extra, start(at, offset) { this.startAt = at; this.offset = offset; }, stop(at) { this.stopAt = at; } });
      log.sources.push(source);
      return source;
    }
    resume() { this.state = 'running'; return Promise.resolve(); }
  }
  return { scope: { AudioContext: FakeContext, navigator: { userActivation: { hasBeenActive: true } } }, log, FakeContext };
}

// 极简选择器匹配:支持 tag / .class / [attr] / [attr="v"] 的复合选择器与逗号列表,
// 足够覆盖 themeSoundForTarget 用到的选择器。
function fakeElement({ tag = 'div', classes = [], attrs = {}, parent = null, disabled = false } = {}) {
  const element = {
    tag, classes: new Set(classes), attrs, parentElement: parent, disabled,
    getAttribute: (name) => (name in attrs ? String(attrs[name]) : null),
    matches(list) {
      return list.split(',').map((s) => s.trim()).some((selector) => {
        const tagMatch = selector.match(/^[a-z]+/);
        if (tagMatch && tagMatch[0] !== tag) return false;
        for (const [, cls] of selector.matchAll(/\.([\w-]+)/g)) if (!element.classes.has(cls)) return false;
        for (const [, name, value] of selector.matchAll(/\[([\w-]+)(?:="([^"]*)")?\]/g)) {
          if (!(name in attrs)) return false;
          if (value !== undefined && String(attrs[name]) !== value) return false;
        }
        return true;
      });
    },
    closest(list) {
      for (let current = element; current; current = current.parentElement) if (current.matches(list)) return current;
      return null;
    },
  };
  return element;
}

// 场景:每个音效名在每个风格里都有配方,配方参数在安全范围内。
// 期望:音色只用 Web Audio 自带波形与滤波器类型;峰值增益不超过 0.3(主总线还会再压一道);
// 点击类短音不超过 120ms,其它不超过 1.3 秒 —— 高频操作的提示音一长就变成噪音。
await run('every sound has a bounded recipe in every style', () => {
  assert.deepEqual([...THEME_SOUND_STYLES], Object.keys(THEME_SOUND_RECIPES));
  const waves = new Set(['sine', 'square', 'sawtooth', 'triangle']);
  const filters = new Set(['lowpass', 'highpass', 'bandpass']);
  for (const style of THEME_SOUND_STYLES) {
    for (const name of THEME_SOUND_NAMES) {
      const recipe = THEME_SOUND_RECIPES[style][name];
      assert.ok(recipe && recipe.voices.length > 0, `${style}.${name}`);
      assert.ok(recipe.level > 0 && recipe.level <= 1.5, `${style}.${name} level`);
      for (const voice of recipe.voices) {
        assert.ok(voice.kind === 'tone' || voice.kind === 'noise', `${name} kind`);
        assert.ok(voice.at >= 0 && voice.dur > 0 && voice.gain > 0 && voice.gain <= 0.3, `${name} voice ${JSON.stringify(voice)}`);
        if (voice.kind === 'tone') {
          assert.ok(waves.has(voice.wave) && voice.from >= 20 && voice.from <= 12000, `${name} tone`);
          if (voice.to) assert.ok(voice.to >= 20 && voice.to <= 12000, `${name} tone end`);
        }
        if (voice.filter) assert.ok(filters.has(voice.filter.type) && voice.filter.freq > 0, `${name} filter`);
      }
      const limit = ['click', 'select', 'toggle'].includes(name) ? 0.12 : 1.3;
      assert.ok(themeSoundDuration(recipe) <= limit, `${name} lasts ${themeSoundDuration(recipe)}s`);
    }
  }
});

// 场景:主题包用顶层 "sounds" 声明音效(旧客户端与工坊只当作未知键忽略)。
// 期望:只认识已知风格;缺省、数组、字符串、未知风格一律视为没有音效,不出静音按钮。
await run('packages declare sounds with a known style only', () => {
  assert.equal(themeSoundStyle({ sounds: { style: 'hud' } }), 'hud');
  for (const sounds of [undefined, null, 'hud', ['hud'], {}, { style: 'arcade' }]) {
    assert.equal(themeSoundStyle({ sounds }), null, JSON.stringify(sounds));
  }
  assert.equal(themeSoundStyle(null), null);
});

// 场景:没有声明音效的主题 / 静音 / 已开启。
// 期望:未配置风格或静音时一律不出声;force 只越过静音,不能让无音效主题出声;
// 未知音效名不出声。回归意义:其它主题不能被这套音效影响。
await run('sounds play only for a sound theme that is not muted', async () => {
  resetThemeSoundsForTest();
  const { scope, log } = fakeAudioScope();
  assert.equal(playThemeSound('click', { scope }), false);
  assert.equal(playThemeSound('click', { scope, force: true }), false);
  configureThemeSounds({ style: 'hud', muted: true });
  assert.equal(themeSoundsEnabled(), false);
  assert.equal(playThemeSound('click', { scope }), false);
  assert.equal(log.sources.length, 0);
  assert.equal(playThemeSound('toggle', { scope, force: true }), true);
  configureThemeSounds({ style: 'hud', muted: false });
  assert.equal(themeSoundsEnabled(), true);
  assert.equal(playThemeSound('complete', { scope }), true);
  assert.equal(playThemeSound('no-such-sound', { scope }), false);
  configureThemeSounds({ style: 'arcade', muted: false });
  assert.equal(themeSoundsEnabled(), false);
  assert.equal(playThemeSound('alert', { scope }), false);
  assert.ok(log.sources.length > 0);
});

// 场景:浏览器还没有任何用户操作时(启动瞬间的开机音)。
// 期望:不创建 AudioContext、不出声 —— 此时建的上下文是挂起的,之后会补成一声迟到的响。
await run('no audio context before the first user gesture', () => {
  resetThemeSoundsForTest();
  const { scope, log } = fakeAudioScope();
  scope.navigator.userActivation.hasBeenActive = false;
  configureThemeSounds({ style: 'hud', muted: false });
  assert.equal(playThemeSound('boot', { scope }), false);
  assert.equal(log.sources.length, 0);
});

// 场景:同一个音效在很短时间内被连续触发(连点按钮、批量事件)。
// 期望:点击 40ms 内只响一次,开机音 1.2 秒内只响一次,不同音效互不影响。
await run('throttle spaces repeated sounds', () => {
  let now = 1000;
  const throttle = createThemeSoundThrottle(() => now);
  assert.equal(throttle.allow('click'), true);
  now += 20;
  assert.equal(throttle.allow('click'), false);
  assert.equal(throttle.allow('select'), true);
  now += 25;
  assert.equal(throttle.allow('click'), true);
  assert.equal(throttle.allow('boot'), true);
  now += 1000;
  assert.equal(throttle.allow('boot'), false);
  now += 300;
  assert.equal(throttle.allow('boot'), true);
});

// 场景:一个配方被调度到音频上下文(在线播放与离线渲染共用同一段代码)。
// 期望:每个声部都创建一个音源,开始 / 结束时间按配方偏移;返回的结束时间覆盖最后一个声部;
// 包络从近零起、指数衰减回近零(否则会有爆音)。
await run('scheduling follows the recipe timing', () => {
  const { log, FakeContext } = fakeAudioScope();
  const ctx = new FakeContext();
  const recipe = THEME_SOUND_RECIPES.hud.send;
  const end = scheduleThemeSound(ctx, ctx.destination, recipe, 2);
  assert.equal(log.sources.length, recipe.voices.length);
  recipe.voices.forEach((voice, index) => {
    assert.ok(Math.abs(log.sources[index].startAt - (2 + voice.at)) < 1e-9);
    assert.ok(Math.abs(log.sources[index].stopAt - (2 + voice.at + voice.dur + 0.02)) < 1e-9);
  });
  assert.ok(Math.abs(end - (2 + themeSoundDuration(recipe) + 0.02)) < 1e-9);
  const envelope = log.gains[1].gain.events;
  assert.deepEqual(envelope[0].slice(0, 2), ['set', 0.0001]);
  assert.equal(envelope.at(-1)[0], 'exp');
  assert.equal(envelope.at(-1)[1], 0.0001);
});

// 场景:鼠标按下的目标元素决定音效。
// 期望:普通按钮 → click;会话行、问题导航菱形、页签、菜单项 → select;开关 → toggle;
// 行内的小按钮按最近的可交互元素算 click;发送按钮、风格选择、静音按钮自己出声(这里不响);
// 禁用的按钮与普通文本不出声。
await run('pointer targets map to interaction sounds', () => {
  const button = fakeElement({ tag: 'button' });
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'span', parent: button })), 'click');
  const row = fakeElement({ classes: ['ace-sidebar-session-row'] });
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'span', parent: row })), 'select');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', parent: row })), 'click');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', classes: ['ace-conversation-turn-scrubber-hit'] })), 'select');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', attrs: { role: 'tab' } })), 'select');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', attrs: { role: 'switch' } })), 'toggle');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'input', attrs: { type: 'checkbox' } })), 'toggle');
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'a', attrs: { href: '#x' } })), 'click');
  const send = fakeElement({ tag: 'button', classes: ['ace-composer-send'] });
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'svg', parent: send })), null);
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', attrs: { 'data-theme-variant-option': 'red' } })), null);
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', attrs: { 'data-theme-sound-toggle': 'true' } })), null);
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', disabled: true })), null);
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'button', attrs: { 'aria-disabled': 'true' } })), null);
  assert.equal(themeSoundForTarget(fakeElement({ tag: 'p' })), null);
  assert.equal(themeSoundForTarget(null), null);
});

// 场景:安装全局交互监听后,左键按下按钮、右键、键盘 Enter / Space、按住不放的重复键。
// 期望:左键与键盘激活出声;右键与重复键不出声;卸载后不再监听。
await run('interaction listener plays on primary presses and keyboard activation', () => {
  resetThemeSoundsForTest();
  const { scope, log } = fakeAudioScope();
  globalThis.AudioContext = scope.AudioContext;
  configureThemeSounds({ style: 'hud', muted: false });
  const listeners = {};
  const target = {
    addEventListener: (type, fn) => { listeners[type] = fn; },
    removeEventListener: (type, fn) => { if (listeners[type] === fn) delete listeners[type]; },
  };
  const cleanup = installThemeSoundInteractions(target);
  const button = fakeElement({ tag: 'button' });
  listeners.pointerdown({ button: 2, target: button });
  assert.equal(log.sources.length, 0);
  listeners.pointerdown({ button: 0, isPrimary: true, target: button });
  const afterClick = log.sources.length;
  assert.ok(afterClick > 0);
  listeners.keydown({ key: 'Enter', repeat: true, target: button });
  assert.equal(log.sources.length, afterClick);
  listeners.keydown({ key: ' ', repeat: false, target: fakeElement({ tag: 'button', attrs: { role: 'switch' } }) });
  assert.ok(log.sources.length > afterClick);
  cleanup();
  assert.deepEqual(Object.keys(listeners), []);
  delete globalThis.AudioContext;
});

// 场景:界面接线。期望:静音按钮只在主题声明音效时渲染,并带 data-theme-sound-toggle;
// ThemeProvider 挂音效层;发送 / 排队、停止(ChatView 的 abort,快捷键停止也响)、回合完成、提问与权限、出错都接了音效,
// 且完成、提示、出错只认实时事件(回放帧不重复出声);偏好随 ui-preferences 落盘。
await run('title bar, provider and event sources are wired', () => {
  const topBar = read('../components/TopBar.jsx');
  assert.match(topBar, /\{themeSoundStyle && \(/);
  assert.match(topBar, /data-theme-sound-toggle="true"/);
  const provider = read('../theme.jsx');
  assert.match(provider, /<ThemeSoundLayer[\s\S]*style=\{soundStyle\}[\s\S]*muted=\{!themeSounds\}/);
  const inputBar = read('../components/InputBar.jsx');
  assert.match(inputBar, /playThemeSound\(currentAction\.mode === 'queue' \? 'queue' : 'send'\)/);
  assert.match(read('../components/ChatView.jsx'), /sendAbort\(sid\)\) \{[\s\S]*?\}\s*playThemeSound\('stop'\)/);
  const app = read('../App.jsx');
  assert.match(app, /type === 'completion' && msg\.replayed !== true\) playThemeSound\('complete'\)/);
  assert.match(app, /if \(msg\.replayed === true\) return;[\s\S]*playThemeSound\('alert'\)/);
  assert.match(app, /msg\.type === 'error'[^\n]*msg\.replayed !== true\) \{\s*playThemeSound\('error'\)/);
  assert.match(app, /changeAppearance\(\{ themeSounds: next \}\)/);
  assert.match(read('./appearancePreferences.js'), /theme_sounds: normalized\.themeSounds/);
});
