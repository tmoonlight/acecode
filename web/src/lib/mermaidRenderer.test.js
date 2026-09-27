import assert from 'node:assert/strict';
import { DOMParser, XMLSerializer } from '@xmldom/xmldom';
import mermaid from 'mermaid';
import { colorContrast } from './colorContrast.js';

import {
  createMermaidRenderAdapter,
  inspectMermaidSource,
  installMermaidRenderer,
  isSafeMermaidResourceValue,
  MAX_MERMAID_SOURCE_BYTES,
  mermaidConfig,
  mermaidTheme,
  readMermaidSvgDimensions,
  sanitizeMermaidSvg,
} from './mermaidRenderer.js';
import { renderMarkdown, renderMarkdownBlocks } from './markdown.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const samples = {
  flowchart: 'flowchart TD\nA[Start] --> B{Ready?}\nB -->|Yes| C[Done]',
  state: 'stateDiagram-v2\n[*] --> Ready\nReady --> [*]',
  class: 'classDiagram\nclass Animal {\n  +String name\n}',
  er: 'erDiagram\nUSER ||--o{ ORDER : places',
  sequence: 'sequenceDiagram\nAlice->>Bob: Hello\nBob-->>Alice: Hi',
};

const reportedLabelBreakFlowchart = [
  'graph TB',
  '  RD[std::random_device<br/>真随机种子] --> MT[std::mt19937<br/>梅森旋转算法]',
  '  MT --> UNI[std::uniform_int_distribution<br/>均匀分布整数]',
].join('\n');

const reportedStyledHttpFlowchart = [
  'flowchart LR',
  '    U[🧑 用户<br/>点击按钮] --> B[🌐 浏览器<br/>Chrome / Firefox]',
  '    B --> D[📡 DNS 解析<br/>域名 → IP]',
  '    D --> C[🔗 TCP 三次握手<br/>SYN → SYN-ACK → ACK]',
  '    C --> T[🔒 TLS 握手<br/>证书验证 + 密钥交换]',
  '    T --> L[⚖️ 负载均衡<br/>Nginx / HAProxy]',
  '    L --> W1[🖥️ Web 服务器 1]',
  '    L --> W2[🖥️ Web 服务器 2]',
  '    L --> W3[🖥️ Web 服务器 3]',
  '    W1 --> A[📋 API 网关<br/>路由 / 限流 / 鉴权]',
  '    A --> S[🧠 应用服务<br/>业务逻辑]',
  '    S --> M[🗄️ 缓存层<br/>Redis]',
  '    S --> DB[💾 数据库<br/>MySQL / PostgreSQL]',
  '    S --> Q[📤 消息队列<br/>Kafka / RabbitMQ]',
  '    M --> S',
  '    DB --> S',
  '    Q --> S',
  '    S --> R[JSON 响应]',
  '    R --> U',
  '',
  '    subgraph 客户端[客户端 Client]',
  '        U',
  '        B',
  '        D',
  '    end',
  '',
  '    subgraph 传输[传输层 Transport]',
  '        C',
  '        T',
  '    end',
  '',
  '    subgraph 服务端[服务端 Server]',
  '        L',
  '        W1',
  '        W2',
  '        W3',
  '        A',
  '        S',
  '        M',
  '        DB',
  '        Q',
  '    end',
  '',
  '    style 客户端 fill:#e1f5fe,stroke:#0288d1',
  '    style 传输 fill:#fff3e0,stroke:#f57c00',
  '    style 服务端 fill:#e8f5e9,stroke:#388e3c',
].join('\n');

const completeMermaid = [
  '```MerMaid theme=dark',
  samples.flowchart,
  '```',
].join('\n');

await test('complete Mermaid fence keeps escaped copy source and a Web render target', () => {
  const html = renderMarkdown(completeMermaid);
  assert.match(html, /data-mermaid-diagram="source"/);
  assert.match(html, /data-mermaid-render-target="true"/);
  assert.match(html, /data-mermaid-source="true"/);
  assert.match(html, /data-code-copy-source="true"/);
  assert.ok(!html.includes('<svg'));

  const escaped = renderMarkdown([
    '```mermaid',
    'flowchart TD',
    'A[Safe <script>label</script>] --> B',
    '```',
  ].join('\n'));
  assert.match(escaped, /Safe &lt;script&gt;label&lt;\/script&gt;/);
  assert.ok(!escaped.includes('<script>label</script>'));
});

await test('incomplete and blank Mermaid fences remain ordinary source blocks', () => {
  const incomplete = renderMarkdown(['```mermaid', samples.flowchart].join('\n'));
  assert.ok(!incomplete.includes('data-mermaid-diagram='));
  assert.match(incomplete, /data-code-copy-frame="true"/);

  const blank = renderMarkdown(['```mermaid', '   ', '```'].join('\n'));
  assert.ok(!blank.includes('data-mermaid-diagram='));
});

await test('Mermaid block rendering stays byte-identical to whole Markdown rendering', () => {
  const source = `Before\n\n${completeMermaid}\n\nAfter`;
  const blocks = renderMarkdownBlocks(source);
  assert.equal(blocks.map((block) => block.html).join(''), renderMarkdown(source));
});

await test('source gate accepts exactly the intended five Mermaid families', () => {
  for (const [family, source] of Object.entries(samples)) {
    assert.deepEqual(inspectMermaidSource(source), { ok: true, family, source });
  }
  assert.equal(inspectMermaidSource('pie\ntitle Pets').reason, 'unsupported-family');
  assert.equal(inspectMermaidSource('gitGraph\ncommit').reason, 'unsupported-family');
});

await test('source gate accepts only attribute-free Mermaid label line breaks', () => {
  for (const lineBreak of ['<br>', '<br/>', '<br />', '<BR/>']) {
    const source = `flowchart TD\nA[first${lineBreak}second] --> B`;
    assert.deepEqual(inspectMermaidSource(source), {
      ok: true,
      family: 'flowchart',
      source,
    });
  }

  assert.deepEqual(inspectMermaidSource(reportedLabelBreakFlowchart), {
    ok: true,
    family: 'flowchart',
    source: reportedLabelBreakFlowchart,
  });

  const unsafeHtml = [
    'flowchart TD\nA[first<br class="x">second] --> B',
    'flowchart TD\nA[first<br onclick="x">second] --> B',
    'flowchart TD\nA[first<br/><img src=x>second] --> B',
    'flowchart TD\nA[first<script>second</script>] --> B',
    'flowchart TD\nA[first</br>second] --> B',
    'flowchart TD\nA[first&lt;br&gt;second] --> B',
  ];
  for (const source of unsafeHtml) {
    assert.equal(inspectMermaidSource(source).reason, 'html', source);
  }
});

await test('source gate accepts official static Mermaid styling unchanged', () => {
  const reusableStyles = [
    'flowchart TD',
    'A:::hot --> B',
    'classDef hot fill:#f00,stroke:#333',
    'class B hot',
    'cssClass A hot',
    'linkStyle 0 stroke:#00f,stroke-width:2px',
    'style A color:#111',
  ].join('\n');

  for (const source of [reportedStyledHttpFlowchart, reusableStyles]) {
    assert.deepEqual(inspectMermaidSource(source), {
      ok: true,
      family: 'flowchart',
      source,
    });
  }
});

await test('source gate rejects interaction and unsafe CSS before Mermaid', () => {
  const interactive = [
    'flowchart TD\nA --> B\nclick A callback',
    'flowchart TD\nA --> B\nhref A target',
    'flowchart TD\nA --> B\ncall A handler',
    'sequenceDiagram\nlinks Alice: {"Home": "/home"}',
  ];
  for (const source of interactive) {
    assert.equal(inspectMermaidSource(source).reason, 'interactive', source);
  }

  const unsafeStyles = [
    'flowchart TD\nA --> B\nstyle A width:expression(alert(1))',
    'flowchart TD\nA --> B\nstyle A behavior:unsafe',
    'flowchart TD\nA --> B\nstyle A -moz-binding:none',
    'flowchart TD\nA --> B\nclassDef hot @import "theme.css"',
    'flowchart TD\nA --> B\nclassDef hot @font-face',
  ];
  for (const source of unsafeStyles) {
    assert.equal(inspectMermaidSource(source).reason, 'unsafe-style', source);
  }
  assert.equal(
    inspectMermaidSource('flowchart TD\nA --> B\nstyle A fill:url(#paint)').reason,
    'external-resource',
  );
});

await test('source gate rejects directives, active syntax, unsafe HTML, and resources', () => {
  const rejected = [
    '---\nconfig:\n  theme: dark\n---\nflowchart TD\nA --> B',
    '%%{init: {"theme": "dark"}}%%\nflowchart TD\nA --> B',
    'flowchart TD\nA@{ img: "https://example.test/x.png" }',
    'flowchart TD\nA[<img src=x>] --> B',
    'flowchart TD\nA --> B\nclick A "https://example.test"',
    'flowchart TD\nA[$$x^2$$] --> B',
    'sequenceDiagram\nAlice->>Bob: ![image](asset.png)',
  ];
  for (const source of rejected) assert.equal(inspectMermaidSource(source).ok, false, source);

  const oversized = `flowchart TD\nA[${'x'.repeat(MAX_MERMAID_SOURCE_BYTES)}]`;
  assert.equal(inspectMermaidSource(oversized).reason, 'too-large');
});

await test('official package recognizes all five gated families', () => {
  mermaid.initialize(mermaidConfig('light'));
  const expected = ['flowchart-v2', 'stateDiagram', 'class', 'er', 'sequence'];
  assert.deepEqual(Object.values(samples).map((source) => mermaid.detectType(source)), expected);
});

await test('theme configuration fixes strict classic Dagre rendering', () => {
  const light = mermaidConfig('light');
  const dark = mermaidConfig('dark');
  assert.equal(light.theme, 'default');
  assert.equal(dark.theme, 'dark');
  assert.equal(light.securityLevel, 'strict');
  assert.equal(light.htmlLabels, false);
  assert.equal(light.look, 'classic');
  assert.equal(light.layout, 'dagre');
  const fake = (theme) => ({ documentElement: { getAttribute: () => theme } });
  assert.equal(mermaidTheme(fake('dark')), 'dark');
  assert.equal(mermaidTheme(fake('light')), 'light');
  assert.equal(mermaidTheme(null), 'light');
});

function themeDocument(mode, ...backgrounds) {
  const root = { background: backgrounds.pop(), getAttribute: () => mode };
  let frame = root;
  for (const background of backgrounds.reverse()) frame = { background, parentElement: frame };
  return {
    doc: { documentElement: root, body: frame, defaultView: { getComputedStyle: (node) => ({ backgroundColor: node.background }) } },
    frame,
  };
}

await test('diagram theme follows actual local background instead of declared mode', () => {
  for (const [mode, colors, expected] of [
    ['dark', ['rgba(0, 0, 0, 0)', 'rgb(255, 241, 235)', '#111111'], 'light'],
    ['light', ['rgb(24, 26, 30)', '#ffffff'], 'dark'],
    ['dark', ['#fff', '#111'], 'light'],
    ['light', ['rgb(0% 0% 0%)', '#fff'], 'dark'],
  ]) {
    const { doc, frame } = themeDocument(mode, ...colors);
    assert.equal(mermaidTheme(doc, frame), expected);
    assert.equal(mermaidTheme(doc), expected);
  }
});

await test('diagram theme composites translucent ancestors in paint order', () => {
  for (const [colors, expected] of [
    [['rgba(0, 0, 0, 0.8)', '#ffffff'], 'dark'],
    [['rgba(255, 255, 255, 0.8)', '#000000'], 'light'],
    [['rgba(0, 0, 0, 0.2)', 'rgba(255, 255, 255, 0.8)', '#000000'], 'light'],
    [['rgb(0 0 0 / 80%)', '#fff'], 'dark'],
    [['transparent', 'rgb(255, 241, 235)'], 'light'],
  ]) {
    const { doc, frame } = themeDocument('dark', ...colors);
    assert.equal(mermaidTheme(doc, frame), expected);
  }
  const { doc, frame } = themeDocument('dark', 'transparent');
  assert.equal(mermaidTheme(doc, frame), 'dark');
  doc.defaultView.getComputedStyle = () => { throw new Error('unavailable'); };
  assert.equal(mermaidTheme(doc, frame), 'dark');
});

await test('default diagram ink stays readable across light, dark, tinted and midtone backgrounds', () => {
  for (const background of ['#fff1eb', '#ffffff', '#101820', '#333333', '#767676', '#808080', '#db2020', '#007f80', '#ffff00']) {
    const { doc, frame } = themeDocument('dark', background);
    const config = mermaidConfig(mermaidTheme(doc, frame));
    mermaid.initialize(config);
    const colors = mermaid.mermaidAPI.getConfig().themeVariables;
    for (const name of ['actorLineColor', 'signalColor', 'signalTextColor', 'loopTextColor', 'labelBoxBorderColor', 'lineColor', 'defaultLinkColor', 'transitionColor', 'textColor']) {
      assert.ok(colorContrast(background, colors[name]) >= 4.5, `${background}: ${name}=${colors[name]}`);
    }
    for (const [fill, ink] of [['actorBkg', 'actorTextColor'], ['nodeBkg', 'primaryTextColor']]) {
      assert.ok(colorContrast(colors[fill], colors[ink]) >= 4.5, `${fill}: ${colors[fill]}, ${ink}: ${colors[ink]}`);
    }
  }
});

await test('resource validator allows local fragments and blocks external CSS references', () => {
  assert.equal(isSafeMermaidResourceValue('marker-end:url(#arrowhead)'), true);
  assert.equal(isSafeMermaidResourceValue('@keyframes dash { to { stroke-dashoffset: 0; } }', { css: true }), true);
  assert.equal(isSafeMermaidResourceValue('fill:url(https://example.test/a.svg#x)'), false);
  assert.equal(isSafeMermaidResourceValue('fill:url(blob:https://example.test/id)'), false);
  assert.equal(isSafeMermaidResourceValue('@import "theme.css"', { css: true }), false);
  assert.equal(isSafeMermaidResourceValue('background:url(data:image/svg+xml,abc)'), false);
});

const safeSvg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 160 80" width="100%"><style>@keyframes dash { to { stroke-dashoffset: 0; } } .edge{marker-end:url(#arrow);}</style><defs><marker id="arrow"><path d="M0 0L4 2L0 4Z"/></marker><symbol id="actor"><rect width="20" height="10"/></symbol></defs><g><rect width="80" height="30"/><path class="edge" marker-end="url(#arrow)" d="M80 15L150 15"/><text><tspan>Safe</tspan></text></g></svg>';

await test('SVG sanitizer accepts inert Mermaid vocabulary and bounded viewBox dimensions', () => {
  const result = sanitizeMermaidSvg(safeSvg, { DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });
  assert.equal(result.width, 160);
  assert.equal(result.height, 80);
  assert.match(result.svg, /^<svg/);

  const documentNode = new DOMParser().parseFromString(safeSvg, 'image/svg+xml');
  assert.deepEqual(readMermaidSvgDimensions(documentNode.documentElement), { width: 160, height: 80 });
});

await test('SVG sanitizer fails closed on active markup, foreign resources, and dimensions', () => {
  const sanitize = (svg) => sanitizeMermaidSvg(svg, {
    DOMParserType: DOMParser,
    XMLSerializerType: XMLSerializer,
  });
  assert.equal(sanitize(safeSvg.replace('<g>', '<script>alert(1)</script><g>')), null);
  assert.equal(sanitize(safeSvg.replace('<g>', '<foreignObject><div xmlns="http://www.w3.org/1999/xhtml">x</div></foreignObject><g>')), null);
  assert.equal(sanitize(safeSvg.replace('<rect ', '<image href="https://example.test/x" ')), null);
  assert.equal(sanitize(safeSvg.replace('<rect ', '<rect onload="alert(1)" ')), null);
  assert.equal(sanitize(safeSvg.replace('url(#arrow)', 'url(https://example.test/a.svg#arrow)')), null);
  assert.equal(sanitize(safeSvg.replace('0 0 160 80', '0 0 40000 80')), null);
});

await test('render adapter submits safe label breaks and gates other HTML before Mermaid', async () => {
  const calls = [];
  const runtime = {
    initialize() { calls.push(['initialize']); },
    async parse(source) { calls.push(['parse', source]); return true; },
    async render(id, source) { calls.push(['render', id, source]); return { svg: safeSvg }; },
  };
  const render = createMermaidRenderAdapter({
    runtime,
    DOMParserType: DOMParser,
    XMLSerializerType: XMLSerializer,
  });

  const result = await render(reportedLabelBreakFlowchart, 'light');
  assert.equal(result.width, 160);
  assert.equal(calls.find((call) => call[0] === 'parse')[1], reportedLabelBreakFlowchart);
  assert.equal(calls.find((call) => call[0] === 'render')[2], reportedLabelBreakFlowchart);

  const before = calls.length;
  assert.equal(await render('flowchart TD\nA[safe<br/><img src=x>unsafe] --> B', 'light'), null);
  assert.equal(calls.length, before);
});

await test('render adapter submits the exact styled HTTP flowchart and gates unsafe styles', async () => {
  const calls = [];
  const runtime = {
    initialize() { calls.push(['initialize']); },
    async parse(source) { calls.push(['parse', source]); return true; },
    async render(id, source) { calls.push(['render', id, source]); return { svg: safeSvg }; },
  };
  const render = createMermaidRenderAdapter({
    runtime,
    DOMParserType: DOMParser,
    XMLSerializerType: XMLSerializer,
  });

  const result = await render(reportedStyledHttpFlowchart, 'light');
  assert.equal(result.width, 160);
  assert.equal(calls.find((call) => call[0] === 'parse')[1], reportedStyledHttpFlowchart);
  assert.equal(calls.find((call) => call[0] === 'render')[2], reportedStyledHttpFlowchart);

  const before = calls.length;
  assert.equal(await render('flowchart TD\nA --> B\nstyle A behavior:unsafe', 'light'), null);
  assert.equal(calls.length, before);
});

await test('serialized render adapter gates before Mermaid and preserves fallback on failure', async () => {
  const calls = [];
  const runtime = {
    initialize(config) { calls.push(['initialize', config.theme]); },
    async parse(source) { calls.push(['parse', source]); return true; },
    async render(id, source) { calls.push(['render', id, source]); return { svg: safeSvg }; },
  };
  const render = createMermaidRenderAdapter({
    runtime,
    DOMParserType: DOMParser,
    XMLSerializerType: XMLSerializer,
  });
  const result = await render(samples.flowchart, 'dark');
  assert.equal(result.width, 160);
  assert.deepEqual(calls.map((call) => call[0]), ['initialize', 'parse', 'render']);
  assert.equal(calls[0][1], 'dark');

  const before = calls.length;
  assert.equal(await render('pie\ntitle Unsafe family', 'light'), null);
  assert.equal(calls.length, before);

  runtime.render = async () => { throw new Error('render failed'); };
  assert.equal(await render(samples.state, 'light'), null);
});

const tick = () => new Promise((resolve) => setImmediate(resolve));

function rendererFixture({ deferred = false } = {}) {
  const element = () => ({
    attributes: new Map(), style: {}, children: [], listeners: new Map(),
    setAttribute(name, value) { this.attributes.set(name, value); },
    getAttribute(name) { return this.attributes.get(name) ?? null; },
    removeAttribute(name) { this.attributes.delete(name); },
    addEventListener(name, callback) { this.listeners.set(name, callback); },
    emit(name) { this.listeners.get(name)?.(); },
    append(child) { this.children.push(child); child.parentElement = this; },
    replaceChildren(...children) { this.children = []; children.forEach((child) => this.append(child)); },
    contains(child) {
      for (let node = child; node; node = node.parentElement) if (node === this) return true;
      return false;
    },
  });
  const root = element(), panel = element(), frame = element(), target = element();
  root.setAttribute('data-theme', 'dark');
  root.background = '#111111';
  panel.background = '#fff1eb';
  root.append(panel);
  panel.append(frame);
  frame.append(target);
  frame.setAttribute('data-mermaid-diagram', 'source');
  frame.querySelector = (selector) => selector.includes('render-target') ? target : { textContent: samples.sequence };
  let mutations, observed;
  const transitions = new Map();
  const doc = {
    addEventListener: (name, callback) => transitions.set(name, callback),
    removeEventListener: (name) => transitions.delete(name),
    documentElement: root, body: panel,
    createElement: element,
    querySelectorAll: (selector) => selector.includes('="source"') && frame.getAttribute('data-mermaid-diagram') !== 'source' ? [] : [frame],
  };
  const calls = [], events = [];
  const win = {
    Blob, DOMParser, XMLSerializer,
    URL: { createObjectURL: () => 'blob:fixture', revokeObjectURL() {} },
    getComputedStyle: (node) => ({ backgroundColor: node.background || 'rgba(0, 0, 0, 0)' }),
    MutationObserver: class {
      constructor(callback) { mutations = callback; }
      observe(node, options) { observed = options; }
      disconnect() { mutations = null; }
    },
    CustomEvent: class { constructor(type, { detail }) { this.type = type; this.detail = detail; } },
    dispatchEvent(event) { events.push(event); return true; },
  };
  doc.defaultView = win;
  const result = { svg: safeSvg, width: 160, height: 80 };
  const stop = installMermaidRenderer(win, doc, (source, theme) => {
    let resolve;
    const pending = new Promise((done) => { resolve = () => done(result); });
    calls.push({ source, theme, resolve });
    if (!deferred) resolve();
    return pending;
  });
  return {
    root, panel, frame, target, calls, events, stop,
    image: () => target.children[0]?.children[0],
    transition: (node, type = 'transitionend') => transitions.get(type)?.({ target: node, propertyName: 'background-color' }),
    mutate(node, attributeName) {
      if (observed.attributeFilter.includes(attributeName)) mutations?.([{ type: 'attributes', target: node, attributeName }]);
    },
  };
}

await test('renderer refreshes background-only changes without looping on its own image styles', async () => {
  const page = rendererFixture();
  try {
    await tick();
    page.image().emit('load');
    assert.equal(page.frame.getAttribute('data-mermaid-theme'), 'light');
    assert.deepEqual(page.image().style, { width: '160px', height: '80px' });

    for (const [attribute, node] of [
      ['style', page.panel], ['class', page.panel], ['data-color-theme', page.root],
      ['data-theme-session-background', page.root], ['data-theme', page.root],
    ]) {
      page.panel.background = page.panel.background === '#fff1eb' ? '#111111' : '#fff1eb';
      page.mutate(node, attribute);
      await tick();
      page.image().emit('load');
      assert.equal(page.frame.getAttribute('data-mermaid-theme'), page.panel.background === '#111111' ? 'dark' : 'light');
    }
    const rendered = page.calls.length;
    page.mutate(page.image(), 'style');
    page.mutate(page.root, 'class'); // The background did not change.
    await tick();
    assert.equal(page.calls.length, rendered);
    page.target.children[0].emit('click');
    assert.equal(page.events.at(-1).detail.theme, 'dark');
  } finally { page.stop(); }
});

await test('a late render from the previous background cannot replace the current diagram', async () => {
  const page = rendererFixture({ deferred: true });
  try {
    await tick();
    page.panel.background = '#111111';
    page.mutate(page.panel, 'style');
    await tick();
    assert.deepEqual(page.calls.map((call) => call.theme), ['light', 'dark']);
    page.calls[1].resolve();
    await tick();
    const currentImage = page.image();
    currentImage.emit('load');
    page.calls[0].resolve();
    await tick();
    assert.equal(page.image(), currentImage);
    assert.equal(page.frame.getAttribute('data-mermaid-theme'), 'dark');
  } finally { page.stop(); }
});

await test('image loading rechecks the actual background even before a mutation notification', async () => {
  const page = rendererFixture();
  try {
    await tick();
    const oldImage = page.image();
    page.panel.background = '#111111';
    oldImage.emit('load');
    await tick();
    assert.deepEqual(page.calls.map((call) => call.theme), ['light', 'dark']);
    page.image().emit('load');
    oldImage.emit('load');
    assert.equal(page.frame.getAttribute('data-mermaid-theme'), 'dark');
    assert.notEqual(page.image(), oldImage);
  } finally { page.stop(); }
});

await test('background transitions refresh at their settled color and unsubscribe on cleanup', async () => {
  const page = rendererFixture();
  try {
    await tick();
    page.image().emit('load');
    page.mutate(page.panel, 'style'); // Computed style is still the previous color.
    await tick();
    assert.equal(page.calls.length, 1);
    page.panel.background = '#111111';
    page.transition(page.panel);
    await tick();
    page.image().emit('load');
    assert.equal(page.frame.getAttribute('data-mermaid-theme'), 'dark');

    page.panel.background = '#fff1eb';
    page.transition(page.panel, 'transitioncancel');
    await tick();
    page.image().emit('load');
    assert.equal(page.frame.getAttribute('data-mermaid-theme'), 'light');
  } finally { page.stop(); }
  const rendered = page.calls.length;
  page.panel.background = '#111111';
  page.transition(page.panel);
  await tick();
  assert.equal(page.calls.length, rendered);
});
