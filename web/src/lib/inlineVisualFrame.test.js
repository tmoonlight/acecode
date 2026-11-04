import assert from 'node:assert/strict';
import {
  INLINE_VISUAL_CDN_ORIGINS,
  INLINE_VISUAL_MAX_HEIGHT,
  INLINE_VISUAL_MIN_HEIGHT,
  INLINE_VISUAL_PALETTES,
  INLINE_VISUAL_TONES,
  INLINE_VISUAL_TOKENS,
  buildInlineVisualSrcdoc,
  clampInlineVisualHeight,
  createInlineVisualSendLimiter,
  inlineVisualBaseCss,
  inlineVisualCodeKey,
  inlineVisualCsp,
  inlineVisualLinkUrl,
  inlineVisualMessage,
  inlineVisualSendText,
  inlineVisualThemeCss,
  readInlineVisualTheme,
} from './inlineVisualFrame.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function cspDirectives(csp) {
  return Object.fromEntries(csp.split(';').map((part) => part.trim()).filter(Boolean)
    .map((part) => {
      const [name, ...values] = part.split(/\s+/);
      return [name, values];
    }));
}

function fakeDocument(mode, values) {
  return {
    documentElement: { getAttribute: (name) => (name === 'data-theme' ? mode : null) },
    defaultView: {
      getComputedStyle: () => ({ getPropertyValue: (name) => values[name] || '' }),
    },
  };
}

// 触发场景:框内脚本试图 fetch('http://127.0.0.1:<daemon 端口>/api/sessions'),或用图片 /
// WebSocket 把数据带出去。本机请求免 token,隔离框必须靠内容安全策略挡住。
// 期望行为:默认全部禁止;脚本 / 样式 / 字体 / 网络只放行四个公共 CDN,图片与媒体只能是
// data: / blob:;不能提交表单、不能改 base;任何指令里都没有 'self'、localhost 或 127.0.0.1。
run('内容安全策略只放行四个 CDN,挡住本机 daemon', () => {
  const directives = cspDirectives(inlineVisualCsp());
  assert.deepEqual(directives['default-src'], ["'none'"]);
  assert.deepEqual(directives['connect-src'], [...INLINE_VISUAL_CDN_ORIGINS]);
  assert.deepEqual(directives['script-src'], ["'unsafe-inline'", "'unsafe-eval'", ...INLINE_VISUAL_CDN_ORIGINS]);
  assert.deepEqual(directives['style-src'], ["'unsafe-inline'", ...INLINE_VISUAL_CDN_ORIGINS]);
  assert.deepEqual(directives['img-src'], ['data:', 'blob:']);
  assert.deepEqual(directives['media-src'], ['data:', 'blob:']);
  assert.deepEqual(directives['form-action'], ["'none'"]);
  assert.deepEqual(directives['base-uri'], ["'none'"]);
  const csp = inlineVisualCsp();
  for (const forbidden of ["'self'", 'localhost', '127.0.0.1', '[::1]', 'http:', 'ws:', '*']) {
    assert.equal(csp.includes(forbidden), false, forbidden);
  }
  assert.deepEqual(INLINE_VISUAL_CDN_ORIGINS, [
    'https://cdnjs.cloudflare.com', 'https://cdn.jsdelivr.net', 'https://unpkg.com', 'https://esm.sh',
  ]);
});

// 触发场景:每个 visual 隔离框挂载时生成一次 srcdoc。
// 期望行为:<head> 的第一个元素就是内容安全策略(早于任何样式和脚本,否则它之前的内容不受约束);
// 初始主题直接写进外壳;预置 #arrow / #dot 标记;运行时脚本只有一段且不会被提前闭合 ——
// 即使文案里含 </script> 也不会把后面的内容变成可执行 HTML。
run('srcdoc 以内容安全策略开头,运行时脚本不会被提前闭合', () => {
  const theme = readInlineVisualTheme(fakeDocument('dark', {}));
  const srcdoc = buildInlineVisualSrcdoc({
    theme,
    labels: { scriptFailed: 'x</script><img src=x onerror=alert(1)>' },
  });
  assert.ok(srcdoc.startsWith(
    '<!DOCTYPE html><html data-mode="dark"><head><meta http-equiv="Content-Security-Policy" content="',
  ));
  const cspAt = srcdoc.indexOf('Content-Security-Policy');
  assert.ok(cspAt < srcdoc.indexOf('<style'));
  assert.ok(cspAt < srcdoc.indexOf('<script'));
  assert.equal(srcdoc.split('<script>').length - 1, 1);
  assert.equal(srcdoc.split('</script').length - 1, 1);
  assert.equal(srcdoc.includes('<img src=x'), false);
  assert.ok(srcdoc.trimEnd().endsWith('</script></body></html>'));
  assert.ok(srcdoc.includes('<marker id="arrow"'));
  assert.ok(srcdoc.includes('<marker id="dot"'));
  assert.ok(srcdoc.includes('<div id="ace-root"></div>'));
  assert.ok(srcdoc.includes('<style id="ace-theme">:root{--fg:#bfbfbf;'));
  assert.ok(srcdoc.includes('"mode":"dark"'));
});

// 触发场景:外层读当前 ACECode 主题推给隔离框(挂载时与主题切换时)。
// 期望行为:--ace-* 映射成框内短变量,--fg-3 / --border-strong 由 --fg 推出半透明色;
// 读不到的变量或带 } < ; 的可疑值(奇怪的自定义主题)回落到默认值,不能把规则写进框内样式;
// data-theme="dark" 时 mode 为 dark,调色板换成深色那一套。
run('主题变量映射、派生与可疑值回落', () => {
  const theme = readInlineVisualTheme(fakeDocument('dark', {
    '--ace-fg': '#bfbfbf',
    '--ace-accent': '#3b82f6',
    '--ace-surface': 'red}body{display:none',
    '--font-sans': '"Segoe UI", sans-serif',
  }));
  assert.equal(theme.mode, 'dark');
  assert.equal(theme.vars['--fg'], '#bfbfbf');
  assert.equal(theme.vars['--accent'], '#3b82f6');
  assert.equal(theme.vars['--surface'], '#1a1a1a');
  assert.equal(theme.vars['--font'], '"Segoe UI", sans-serif');
  assert.equal(theme.vars['--fg-3'], 'rgba(191, 191, 191, 0.42)');
  assert.equal(theme.vars['--border-strong'], 'rgba(191, 191, 191, 0.26)');
  assert.deepEqual(theme.palette, [...INLINE_VISUAL_PALETTES.dark]);
  for (const token of INLINE_VISUAL_TOKENS) assert.ok(theme.vars[token], token);

  const light = readInlineVisualTheme(fakeDocument(null, {}));
  assert.equal(light.mode, 'light');
  assert.equal(light.vars['--fg'], '#1a1a1a');

  const css = inlineVisualThemeCss({ '--fg': '#111', '--accent': 'x;}html{color:red', other: 'blue' });
  assert.equal(css, ':root{--fg:#111;}');
});

// 触发场景:模型按画图规范使用色调类与 SVG 辅助类。
// 期望行为:8 种色调各有浅色与深色两套 --tone-* 取值;形状、文字、卡片规则读这些变量;
// 两套调色板各 8 个十六进制颜色,给 canvas 图表库用。
run('色调类、辅助类与调色板齐全', () => {
  const css = inlineVisualBaseCss();
  assert.deepEqual(INLINE_VISUAL_TONES, ['blue', 'teal', 'green', 'amber', 'red', 'purple', 'pink', 'gray']);
  for (const tone of INLINE_VISUAL_TONES) {
    assert.ok(css.includes(`.tone-${tone}{--tone-bg:`), tone);
    assert.ok(css.includes(`[data-mode="dark"] .tone-${tone}{--tone-bg:`), tone);
  }
  for (const rule of ['.node{fill:var(--tone-bg);stroke:var(--tone-line)', '.edge{fill:none;stroke:var(--fg-3)',
    '.t{fill:var(--tone-ink)', '.t-mid{text-anchor:middle', '.card{', '.badge{']) {
    assert.ok(css.includes(rule), rule);
  }
  for (const mode of ['light', 'dark']) {
    assert.equal(INLINE_VISUAL_PALETTES[mode].length, 8);
    for (const color of INLINE_VISUAL_PALETTES[mode]) assert.match(color, /^#[0-9a-f]{6}$/);
  }
});

// 触发场景:框内(或冒充框内的任意页面脚本)向外层 postMessage。
// 期望行为:只认带协议标记、类型已知的消息;来源是否是自己的 iframe 由组件另行校验。
run('框内消息只认协议标记与已知类型', () => {
  assert.equal(inlineVisualMessage({ ace_visual: 1, type: 'size', height: 10 }).type, 'size');
  for (const type of ['ready', 'send', 'link', 'error', 'library']) {
    assert.ok(inlineVisualMessage({ ace_visual: 1, type }), type);
  }
  for (const value of [null, 'x', { type: 'send' }, { ace_visual: 2, type: 'send' },
    { ace_visual: 1, type: 'render' }, { ace_visual: 1, type: 'eval' }]) {
    assert.equal(inlineVisualMessage(value), null, JSON.stringify(value));
  }
});

// 触发场景:框内上报高度(内容撑高、脚本展开、模型写了 100vh 之类导致越撑越高)。
// 期望行为:夹在 40–4000px 之间并取整;非数字 / 非正数忽略。
run('隔离框高度夹取', () => {
  assert.equal(clampInlineVisualHeight(12), INLINE_VISUAL_MIN_HEIGHT);
  assert.equal(clampInlineVisualHeight(520.2), 521);
  assert.equal(clampInlineVisualHeight(99999), INLINE_VISUAL_MAX_HEIGHT);
  for (const value of [0, -5, NaN, 'abc', null, undefined, Infinity]) {
    assert.equal(clampInlineVisualHeight(value), null, String(value));
  }
});

// 触发场景:用户点了框内的追问按钮,或框内脚本塞进奇怪的参数。
// 期望行为:去掉首尾空白;空文本、非字符串、超过 4000 字符的一律忽略。
run('追问文本校验', () => {
  assert.equal(inlineVisualSendText('  讲讲缓存层 '), '讲讲缓存层');
  assert.equal(inlineVisualSendText('x'.repeat(4000)).length, 4000);
  for (const value of ['', '   ', 'x'.repeat(4001), 42, null, {}, ['a']]) {
    assert.equal(inlineVisualSendText(value), null, String(value).slice(0, 20));
  }
});

// 触发场景:框内脚本在用户一次点击里连发追问,或用户快速连点。
// 期望行为:同一个框 1 秒内最多生效一次;被拒的调用不刷新计时。
run('追问限频:每个框 1 秒一次', () => {
  const allow = createInlineVisualSendLimiter(1000);
  assert.equal(allow(10_000), true);
  assert.equal(allow(10_500), false);
  assert.equal(allow(10_999), false);
  assert.equal(allow(11_000), true);
  const other = createInlineVisualSendLimiter(1000);
  assert.equal(other(10_500), true);
});

// 触发场景:用户点了框内链接,或脚本调用 ace.openLink / window.open。
// 期望行为:只有 http / https / mailto 的绝对地址交给系统浏览器;javascript:、file:、data:、
// 相对路径一律忽略。
run('框内链接白名单', () => {
  assert.equal(inlineVisualLinkUrl('https://example.com/a?b=1'), 'https://example.com/a?b=1');
  assert.equal(inlineVisualLinkUrl('http://example.com'), 'http://example.com/');
  assert.equal(inlineVisualLinkUrl('mailto:a@example.com'), 'mailto:a@example.com');
  for (const value of ['javascript:alert(1)', 'file:///C:/x', 'data:text/html,x', '/api/sessions',
    '#top', '', 42, 'https://' + 'x'.repeat(5000)]) {
    assert.equal(inlineVisualLinkUrl(value), null, String(value).slice(0, 30));
  }
});

// 触发场景:同一段 visual 重新挂载(滚回历史、切回会话)时先用上次的高度。
// 期望行为:同一代码得到同一个键,内容不同得到不同的键。
run('高度记忆的键', () => {
  assert.equal(inlineVisualCodeKey('<svg></svg>'), inlineVisualCodeKey('<svg></svg>'));
  assert.notEqual(inlineVisualCodeKey('<svg></svg>'), inlineVisualCodeKey('<svg> </svg>'));
});
