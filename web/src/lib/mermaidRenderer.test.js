import assert from 'node:assert/strict';
import { DOMParser, XMLSerializer } from '@xmldom/xmldom';
import mermaid from 'mermaid';
import { colorContrast } from './colorContrast.js';

import {
  createMermaidRenderAdapter,
  escapeMermaidSequenceText,
  inspectMermaidSource,
  installMermaidRenderer,
  isSafeMermaidResourceValue,
  MAX_MERMAID_SOURCE_BYTES,
  mermaidConfig,
  mermaidFailureNote,
  mermaidTheme,
  neutralizeMermaidMarkup,
  prepareMermaidRenderSource,
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

await test('source gate passes attribute-free Mermaid label line breaks through unchanged', () => {
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

  // 已经编码过的尖括号(HTML 实体)不再拒绝:换成 Mermaid 自己的实体码,与改写原文尖括号是同一个结果。
  const encoded = 'flowchart TD\nA[first&lt;br&gt;second] --> B';
  assert.equal(inspectMermaidSource(encoded).ok, true);
  assert.equal(prepareMermaidRenderSource(encoded, 'flowchart'), 'flowchart TD\nA[first#lt;br#gt;second] --> B');
});

// 场景:讲代码的图在标签里写 `<cachePath>`、`List<String>`、`i<n`,甚至 `<script>` 这类原文。
// 期望:闸门不再整张拒绝;交给 Mermaid 的文本里除了不带属性的 `<br>` 之外,所有能被当成标签的 `<`
// 都改写成 `#lt;`,配成一对的连 `>` 一起改成 `#gt;`,连线箭头 `-->` 不受影响。
// 回归:以前这些图整张退回 Mermaid 源码,用户看到的是一堆没画出来的文字。
await test('tag-shaped label text is neutralized into Mermaid entity codes instead of rejected', () => {
  const cases = [
    ['flowchart TD\nA[i<n] --> B[List<String>]', 'flowchart TD\nA[i#lt;n] --> B[List#lt;String#gt;]'],
    ['flowchart TD\nA -- a<b --> B', 'flowchart TD\nA -- a#lt;b --> B'],
    ['flowchart TD\nA[first<br class="x">second] --> B', 'flowchart TD\nA[first#lt;br class="x"#gt;second] --> B'],
    ['flowchart TD\nA[first</br>second] --> B', 'flowchart TD\nA[first#lt;/br#gt;second] --> B'],
    ['flowchart TD\nA[safe<br/><img src=x>] --> B', 'flowchart TD\nA[safe<br/>#lt;img src=x#gt;] --> B'],
    ['flowchart TD\nA[<script>x</script>] --> B', 'flowchart TD\nA[#lt;script#gt;x#lt;/script#gt;] --> B'],
    ['classDiagram\nclass Repo {\n  +load() Map<K,V>\n}', 'classDiagram\nclass Repo {\n  +load() Map#lt;K,V#gt;\n}'],
  ];
  for (const [source, expected] of cases) {
    assert.deepEqual(inspectMermaidSource(source).ok, true, source);
    assert.equal(neutralizeMermaidMarkup(source), expected, source);
  }

  // 不变式:不管原文怎么嵌套、带不带属性、有没有收尾,改写后都找不到 `<字母` 或 `</字母`(`<br>` 除外)。
  const hostile = [
    'flowchart TD\nA[<img src=x onerror=alert(1)>] --> B',
    'flowchart TD\nA[<a <script>x] --> B',
    'flowchart TD\nA[<svg/onload=alert(1)>] --> B',
    'flowchart TD\nA[x<br onclick="y">z<BR/>w] --> B',
    'sequenceDiagram\nAlice->>Bob: <iframe src=//x></iframe>',
  ];
  for (const source of hostile) {
    const neutral = neutralizeMermaidMarkup(source).replace(/<br[ \t]*\/?>/gi, '');
    assert.equal(/<\/?[A-Za-z]/.test(neutral), false, source);
    assert.equal(inspectMermaidSource(source).ok, true, source);
  }
});

// 场景:时序图消息 / 注释里写了 CSS 或代码,带 `;`(`width:100%;height:auto`)或 `#`(`C#`、`#1`)。
// 期望:只改消息、注释和 loop / alt 分组标题里的文字:`;` → `#59;`、`#` → `#35;`,已有实体码原样保留;
// 同一行出现「style…:…#…;」时行尾垫一个 `;`,给 Mermaid 预处理删(否则它会吃掉实体码的分号)。
// participant 别名等其它语句和其它图类型不动。
// 回归:`;` 让 Mermaid 把后半句当成新语句报语法错误,整张时序图退回源码;`#` 之后的文字被悄悄截掉。
await test('sequence message text escapes semicolons and hashes before Mermaid', () => {
  const source = [
    'sequenceDiagram',
    'participant C as c;d',
    'Alice->>Bob: a;b #1 #59; C#',
    'Note over Alice: img style="width:100%;height:auto"',
    'loop n; #2',
    'Alice-)Bob: done',
    'end',
  ].join('\n');
  assert.equal(escapeMermaidSequenceText(source), [
    'sequenceDiagram',
    'participant C as c;d',
    'Alice->>Bob: a#59;b #35;1 #59; C#35;',
    'Note over Alice: img style="width:100%#59;height:auto";',
    'loop n#59; #35;2',
    'Alice-)Bob: done',
    'end',
  ].join('\n'));

  assert.equal(prepareMermaidRenderSource(source, 'sequence'), escapeMermaidSequenceText(source));
  const flowchart = 'flowchart TD\nA[a;b] --> B';
  assert.equal(prepareMermaidRenderSource(flowchart, 'flowchart'), flowchart);
});

// 场景:用户反馈的两张图(名字已换成通用写法)。状态图的连线标签里有 `src=file://cachePath<br/>…`;
// 时序图的注释里有 `"file://<cachePath>"` 和 `style="width:100%;height:auto"`,消息里有
// `exec("javascript:Editor.insertImage('<URLEncode html>')")`。
// 期望:闸门放行;时序图原文过不了官方解析器(分号),改写后能过;状态图不需要改写(它的解析要
// DOMPurify,Node 里没有 DOM 跑不了,只核对闸门和改写结果)。
// 回归:状态图因为 `file://` 被判成外部资源、时序图因为 `<cachePath>` 被判成 HTML,整张退回源码。
await test('reported code-heavy state and sequence diagrams reach the official parser', async () => {
  const state = [
    'stateDiagram-v2',
    '    [*] --> local: pick image',
    '    local --> uploading: insertImage<br/>src=file://cachePath<br/>data-state=uploading',
    '    uploading --> ready: uploaded<br/>data-preview=?id=saveUrl<br/>src still file://',
    '    uploading --> failed: upload failed<br/>data-state=failed',
    '    ready --> final: send<br/>src=?id=saveUrl',
    '    failed --> final: keep src file://',
    '    final --> [*]',
  ].join('\n');
  const sequence = [
    'sequenceDiagram',
    '    participant TM as Worker(FILE)',
    '    participant RE as RichEditor (Java)',
    '    participant RO as Editor (JS)',
    '    TM->>TM: id = "img_<timestamp>_<random>"',
    '    TM->>TM: copy localPath → getCacheDir()/body_img/<id>.jpg',
    '    TM->>TM: fileUrl = "file://" + cachePath',
    '    RE->>RO: exec("javascript:Editor.insertImage(\'<URLEncode html>\')")',
    '    Note over RO: img:<br/>src="file://<cachePath>"<br/>data-id="<id>"<br/>style="width:100%;height:auto"',
    '    alt preview != null',
    '        RO->>RO: imgs[data-id=id].setAttribute(\'data-state\',\'ready\')',
    '    else preview == null',
    '        RO->>RO: imgs[data-id=id].setAttribute(\'data-state\',\'failed\')',
    '    end',
  ].join('\n');

  assert.deepEqual(inspectMermaidSource(state), { ok: true, family: 'state', source: state });
  assert.equal(prepareMermaidRenderSource(state, 'state'), state);

  assert.deepEqual(inspectMermaidSource(sequence), { ok: true, family: 'sequence', source: sequence });
  mermaid.initialize(mermaidConfig('light'));
  assert.equal(await mermaid.parse(sequence, { suppressErrors: true }), false);
  assert.deepEqual(
    await mermaid.parse(prepareMermaidRenderSource(sequence, 'sequence'), { suppressErrors: true }),
    { diagramType: 'sequence', config: {} },
  );
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

await test('source gate rejects unsafe CSS before Mermaid and drops interaction statements', () => {
  // 点击 / 链接语句不再让整张图退回源码:闸门放行,交给 Mermaid 的文本里这一句换成空行。
  const interactive = [
    ['flowchart TD\nA --> B\nclick A callback', 'flowchart', 'flowchart TD\nA --> B\n'],
    ['sequenceDiagram\nlinks Alice: {"Home": "/home"}\nAlice->>Bob: hi', 'sequence', 'sequenceDiagram\n\nAlice->>Bob: hi'],
  ];
  for (const [source, family, expected] of interactive) {
    assert.equal(inspectMermaidSource(source).ok, true, source);
    assert.equal(prepareMermaidRenderSource(source, family), expected, source);
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

  // 场景:样式语句里的 url() 与标签里调用 getUrl() / saveUrl (...) 的代码文字。
  // 期望:style / classDef / linkStyle 里的 url( 照旧拒绝;标签文字里的不拒绝。
  // 回归:时序图消息 `entity.saveUrl ("/data/x")` 被当成 CSS url(),整张退回源码。
  for (const source of [
    'flowchart TD\nA --> B\nclassDef hot fill:URL(x.svg)',
    'flowchart TD\nA --> B\nlinkStyle 0 stroke:url(x.svg)',
    'stateDiagram-v2\n[*] --> A\n  style A fill:url(x.svg)',
  ]) {
    assert.equal(inspectMermaidSource(source).reason, 'external-resource', source);
  }
  for (const source of [
    'sequenceDiagram\nG-->>H: entity.saveUrl ("/data/uploadtemp/x")',
    'flowchart TD\nA[getUrl()] --> B[new URL(path)]',
  ]) {
    assert.equal(inspectMermaidSource(source).ok, true, source);
  }
});

await test('source gate rejects only what cannot be taken out: image shapes and style resources', () => {
  const rejected = [
    'flowchart TD\nA@{ img: "https://example.test/x.png" }',
    'flowchart TD\nA --> B\nstyle A fill:url(https://example.test/x.svg)',
    '---\nconfig:\n  theme: dark\nflowchart TD\nA --> B',
  ];
  for (const source of rejected) assert.equal(inspectMermaidSource(source).ok, false, source);

  // 以前整张拒绝、现在改写后照画的:front matter、指令、点击语句,以及标签里的 `$$`、`![..](..)`。
  const accepted = [
    '---\nconfig:\n  theme: dark\n---\nflowchart TD\nA --> B',
    '%%{init: {"theme": "dark"}}%%\nflowchart TD\nA --> B',
    'flowchart TD\nA --> B\nclick A "https://example.test"',
    'flowchart TD\nA[$$x^2$$] --> B',
    'sequenceDiagram\nAlice->>Bob: ![image](asset.png)',
  ];
  for (const source of accepted) assert.equal(inspectMermaidSource(source).ok, true, source);

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

// 按 mermaid 11 序列图的真实结构拼:`rect <颜色>` 高亮块与 `box <颜色>` 分组都画成
// <rect class="rect" fill="源码颜色">;参与者框是 class="actor" 的 rect,填充由主题 CSS 管。
function sequenceSvg(blockFills, { family = 'sequence', extra = '' } = {}) {
  const blocks = blockFills.map((fill, index) => `<g><rect x="0" y="${index * 40}" fill="${fill}" width="200" height="30" class="rect"/></g>`).join('');
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 240 200" width="100%" aria-roledescription="${family}">${blocks}<rect x="0" y="160" fill="#eaeaea" width="60" height="30" class="actor actor-top"/>${extra}<text class="messageText">退款</text></svg>`;
}

function blockFills(svg) {
  const root = new DOMParser().parseFromString(svg, 'image/svg+xml').documentElement;
  return Array.from(root.getElementsByTagName('rect')).map((rect) => [rect.getAttribute('class'), rect.getAttribute('fill')]);
}

function rgbHex(fill) {
  const [r, g, b] = fill.match(/\d+/g).map(Number);
  return `#${[r, g, b].map((channel) => channel.toString(16).padStart(2, '0')).join('')}`;
}

const sanitizeWithTheme = (svg, theme, options = {}) => sanitizeMermaidSvg(svg, {
  DOMParserType: DOMParser,
  XMLSerializerType: XMLSerializer,
  theme,
  ...options,
});

// 回归:会话 20260722-091409-e457 的退款序列图写了 `rect rgb(255, 250, 220)`、
// `rect rgb(240, 255, 240)`,登录图写了 `rect rgb(240, 248, 255)`。深色主题下消息文字
// 是纯白,原样画出的浅色高亮块让整段消息白底白字、完全看不见。
// 期望:深色主题把这些块压暗到与白字对比度 ≥ 4.5,且三个通道之间的差值不变(浅黄仍偏黄、
// 浅绿仍偏绿,块与块之间还能区分);浅色主题下它们本来就和黑字对比足够,一个字节都不改。
await test('dark sequence highlight blocks from model-written pastel colors keep white messages readable', () => {
  const pastel = ['rgb(255, 250, 220)', 'rgb(240, 255, 240)', 'rgb(240, 248, 255)', 'rgb(255, 200, 200)'];
  const dark = blockFills(sanitizeWithTheme(sequenceSvg(pastel), 'dark').svg).filter(([name]) => name === 'rect');
  assert.equal(dark.length, pastel.length);
  dark.forEach(([, fill], index) => {
    assert.ok(colorContrast(rgbHex(fill), '#ffffff') >= 4.5, `${pastel[index]} -> ${fill}`);
    const before = pastel[index].match(/\d+/g).map(Number);
    const after = fill.match(/\d+/g).map(Number);
    assert.deepEqual(after.map((channel) => channel - after[0]), before.map((channel) => channel - before[0]), fill);
  });
  assert.equal(dark[0][1], 'rgb(64, 59, 29)');

  const light = sanitizeWithTheme(sequenceSvg(pastel), 'light').svg;
  assert.deepEqual(blockFills(light).filter(([name]) => name === 'rect').map(([, fill]) => fill), pastel);
});

// 场景:mermaid 官方文档推荐的低透明度写法 `rect rgba(0, 0, 255, .1)` 叠在深色画布上本来
// 就是暗色,白字可读,必须原样保留;反过来浅色主题下模型写了深色块(如 rgb(30, 30, 60))
// 配黑字同样看不见,要对称地把块抬亮,且透明度保持不变。
// 阈值:深色画布按 #333333、浅色按 #ffffff 合成后再判对比度,与 mermaidTheme 的兜底画布一致。
await test('translucent tints stay as written and dark blocks lighten under black ink', () => {
  const tint = 'rgba(0, 0, 255, .1)';
  assert.deepEqual(blockFills(sanitizeWithTheme(sequenceSvg([tint]), 'dark').svg)[0], ['rect', tint]);

  const [[, lifted], [, liftedTint]] = blockFills(sanitizeWithTheme(sequenceSvg(['rgb(30, 30, 60)', 'rgba(20, 20, 20, 0.8)']), 'light').svg);
  assert.equal(lifted, 'rgb(208, 208, 238)');
  assert.ok(colorContrast(rgbHex(lifted), '#000000') >= 4.5);
  assert.equal(liftedTint, 'rgba(208, 208, 208, 0.8)');
});

// 场景:只动序列图里 class 含 rect 的背景块。参与者框(class="actor",fill="#eaeaea"
// 会被主题 CSS 覆盖)、其他图类型、无法解析的颜色都不碰;命名色(官方 `box Aqua` 示例)
// 在浏览器里经 canvas 规范化,这里用注入的解析器模拟。不传 theme 时保持旧行为。
await test('background fill correction is limited to parsable sequence highlight blocks', () => {
  const darkActor = blockFills(sanitizeWithTheme(sequenceSvg([]), 'dark').svg);
  assert.deepEqual(darkActor, [['actor actor-top', '#eaeaea']]);

  const flowchart = sanitizeWithTheme(sequenceSvg(['rgb(255, 250, 220)'], { family: 'flowchart-v2' }), 'dark').svg;
  assert.equal(blockFills(flowchart)[0][1], 'rgb(255, 250, 220)');

  const unresolved = sanitizeWithTheme(sequenceSvg(['Aqua']), 'dark', { resolveColor: () => null }).svg;
  assert.equal(blockFills(unresolved)[0][1], 'Aqua');
  const named = sanitizeWithTheme(sequenceSvg(['Aqua', 'transparent']), 'dark', {
    resolveColor: (value) => ({ aqua: '#00ffff', transparent: 'rgba(0, 0, 0, 0)' })[value.toLowerCase()] || null,
  }).svg;
  assert.deepEqual(blockFills(named).slice(0, 2).map(([, fill]) => fill), ['rgb(0, 64, 64)', 'transparent']);

  const untouched = sanitizeMermaidSvg(sequenceSvg(['rgb(255, 250, 220)']), { DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });
  assert.equal(blockFills(untouched.svg)[0][1], 'rgb(255, 250, 220)');
});

// 场景:渲染适配器把当前主题一路传给 SVG 后处理,深色下 mermaid 原样输出的浅色块
// 到达页面前已被压暗;同一份源码在浅色主题下渲染保持原色。
await test('render adapter applies the background correction for the requested theme', async () => {
  const runtime = {
    initialize() {},
    async parse() { return true; },
    async render() { return { svg: sequenceSvg(['rgb(255, 250, 220)']) }; },
  };
  const render = createMermaidRenderAdapter({ runtime, DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });
  assert.equal(blockFills((await render(samples.sequence, 'dark')).svg)[0][1], 'rgb(64, 59, 29)');
  assert.equal(blockFills((await render(samples.sequence, 'light')).svg)[0][1], 'rgb(255, 250, 220)');
});

// 场景:标签里既有安全的 `<br/>`,又有 `<img src=x>` 这类标签原文。
// 期望:`<br/>` 原样交给 Mermaid;其它标签改写成实体码后才交给 Mermaid,原始源码不变。
await test('render adapter submits safe label breaks and neutralizes other HTML before Mermaid', async () => {
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

  calls.length = 0;
  assert.equal((await render('flowchart TD\nA[safe<br/><img src=x>unsafe] --> B', 'light')).width, 160);
  const neutral = 'flowchart TD\nA[safe<br/>#lt;img src=x#gt;unsafe] --> B';
  assert.equal(calls.find((call) => call[0] === 'parse')[1], neutral);
  assert.equal(calls.find((call) => call[0] === 'render')[2], neutral);

  calls.length = 0;
  assert.equal((await render('flowchart TD\nA[first&lt;br&gt;second] --> B', 'light')).width, 160);
  assert.equal(calls.find((call) => call[0] === 'parse')[1], 'flowchart TD\nA[first#lt;br#gt;second] --> B');
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
  assert.deepEqual(
    (await render('flowchart TD\nA --> B\nstyle A behavior:unsafe', 'light')).failure,
    { stage: 'gate', reason: 'unsafe-style', line: 0 },
  );
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
  assert.deepEqual(
    (await render('pie\ntitle Unsafe family', 'light')).failure,
    { stage: 'gate', reason: 'unsupported-family', line: 0 },
  );
  assert.equal(calls.length, before);

  runtime.render = async () => { throw new Error('render failed'); };
  assert.deepEqual(
    (await render(samples.state, 'light')).failure,
    { stage: 'render', reason: 'render failed', line: 0 },
  );
});

// 场景:模型写的流程图标签没加引号却带括号,官方解析器报错。
// 期望:适配器把常见语法小错修一遍(给标签补引号)再解析;修好了就用修过的文本绘制,并在结果上
// 标 repaired;原始源码不变(复制、导出仍用原文)。合法的图不经过修复,交给解析器的就是原文。
// 回归:这类图以前直接显示成源码,用户反馈「mermaid 没转换出来」的多数是这种。
await test('render adapter repairs common syntax slips after a parse failure', async () => {
  const parsed = [];
  const rendered = [];
  const runtime = {
    initialize() {},
    async parse(source) {
      parsed.push(source);
      // 模拟官方解析器:方括号标签里出现没加引号的圆括号就报错。
      if (/\[[^\]"]*\(/.test(source)) throw new Error('Parse error on line 2:\n...A[调用 init() 方法]\n-----^\nExpecting ...');
      return true;
    },
    async render(id, source) { rendered.push(source); return { svg: safeSvg }; },
  };
  const render = createMermaidRenderAdapter({ runtime, DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });

  const broken = 'flowchart TD\nA[调用 init() 方法] --> B';
  const result = await render(broken, 'light');
  assert.equal(result.repaired, true);
  assert.equal(result.width, 160);
  assert.deepEqual(parsed, [broken, 'flowchart TD\nA["调用 init() 方法"] --> B']);
  assert.deepEqual(rendered, ['flowchart TD\nA["调用 init() 方法"] --> B']);

  parsed.length = 0;
  const valid = await render(samples.flowchart, 'light');
  assert.equal(valid.repaired, undefined);
  assert.deepEqual(parsed, [samples.flowchart]);
});

// 场景:修也修不好的语法错误;以及绘制前的模型检查发现了图片地址。
// 期望:语法错误返回 stage=parse,行号按原文算(图前面的 front matter 换成了空行,Mermaid 报的行号
// 要加回去);模型检查不过返回 stage=gate、reason=external-resource,而且绝不调用绘制(绘制时浏览器
// 会当场去取那张图)。
await test('render adapter reports where it failed and never renders a model with resources', async () => {
  const rendered = [];
  let model = {};
  const runtime = {
    initialize() {},
    async parse(source) {
      if (source.includes('???')) throw new Error('Parse error on line 2:\n...B --> ???\n-----^');
      return true;
    },
    mermaidAPI: { async getDiagramFromText() { return { db: model }; } },
    async render(id, source) { rendered.push(source); return { svg: safeSvg }; },
  };
  const render = createMermaidRenderAdapter({ runtime, DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });

  const failed = await render('---\ntitle: x\n---\nflowchart TD\nB --> ???', 'light');
  assert.equal(failed.failure.stage, 'parse');
  assert.equal(failed.failure.line, 5);
  assert.match(failed.failure.reason, /^Parse error on line 2/);

  model = { actors: new Map([['A', { properties: { icon: 'https://example.test/p.png' } }]]) };
  assert.deepEqual(
    (await render(samples.sequence, 'light')).failure,
    { stage: 'gate', reason: 'external-resource', line: 0 },
  );
  assert.deepEqual(rendered, []);

  model = { actors: new Map([['A', { properties: { class: 'hot' } }]]) };
  assert.equal((await render(samples.sequence, 'light')).width, 160);
  assert.equal(rendered.length, 1);

  // 带了 mermaidAPI 却取不到模型:不画。
  runtime.mermaidAPI = {};
  assert.equal((await render(samples.sequence, 'light')).failure.stage, 'render');
  assert.equal(rendered.length, 1);
});

// 场景:画不出来时显示在源码下方的那行说明。问题反馈过来时往往只有一张照片或截图,从图上要能看出卡在哪一层。
// 期望:每一层的说明各不相同;语法错误带得出行号就带;空内容不显示说明。
await test('failure notes tell the layers apart', () => {
  const note = (stage, reason = '', line = 0) => mermaidFailureNote({ stage, reason, line });
  assert.equal(note('parse', 'Parse error', 7), '第 7 行语法有误');
  assert.equal(note('parse'), '图表语法有误');
  assert.equal(note('load'), '图表组件加载失败');
  assert.equal(note('gate', 'unsupported-family'), '暂不支持这种图表类型');
  assert.equal(note('gate', 'too-large'), '图表过大');
  assert.equal(note('gate', 'external-resource'), '图表含有不支持的写法');
  assert.equal(note('gate', 'empty'), '');
  assert.equal(note('sanitize'), '图表绘制结果未通过检查');
  assert.equal(note('render'), '图表绘制失败');
  assert.equal(new Set(['parse', 'load', 'sanitize', 'render'].map((stage) => note(stage))).size, 4);
});

// 场景:流程图 / 类图 / 状态图 / ER 图的标签用 SVG 文字画时,Mermaid 没把 `#quot;`、`#35;`、`#123;`
// 这类实体码还原成字符,图上原样显示 `&quot;`。这正是它的文档教人在标签里写引号的办法,自动修复
// 给标签补引号时也要用到。
// 期望:SVG 检查通过之后,把文字节点里剩下的 `&quot;` `&apos;` `&#十进制;` `&#x十六进制;` 还原成字符;
// 只改文字,不产生标记(还原出来的 `<` 序列化后仍是转义的);控制字符和非法码点原样留着。
await test('SVG post-processing decodes entity codes Mermaid left in label text', () => {
  const svg = safeSvg.replace(
    '<tspan>Safe</tspan>',
    '<tspan>say &amp;quot;hi&amp;quot; &amp;apos;x&amp;apos; C&amp;#35; &amp;#123;a&amp;#x7D; &amp;#60;b&amp;#62; &amp;#0; &amp;#xD800; &amp;lt;</tspan>',
  );
  const result = sanitizeMermaidSvg(svg, { DOMParserType: DOMParser, XMLSerializerType: XMLSerializer });
  const text = new DOMParser().parseFromString(result.svg, 'image/svg+xml').getElementsByTagName('tspan')[0].textContent;
  assert.equal(text, 'say "hi" \'x\' C# {a} <b> &#0; &#xD800; &lt;');
  assert.ok(!result.svg.includes('<b>'));
  assert.equal(new DOMParser().parseFromString(result.svg, 'image/svg+xml').getElementsByTagName('b').length, 0);
});

// 场景:Mermaid 围栏写在引用块里(`> ```mermaid`),或者列表项下面。
// 期望:收尾围栏前面的 `>` 不影响「围栏是否完整」的判断,照样产出可绘制的图框;没写完的围栏
// (回复被截断)仍然是普通代码块。
// 回归:引用块里的图永远被当成没写完,一直显示源码。
await test('Mermaid fences inside blockquotes and lists still become diagram frames', () => {
  const quoted = ['> 流程如下:', '>', '> ```mermaid', '> flowchart TD', '> A --> B', '> ```'].join('\n');
  assert.match(renderMarkdown(quoted), /data-mermaid-diagram="source"/);
  const nested = ['> > ```mermaid', '> > flowchart TD', '> > A --> B', '> > ```'].join('\n');
  assert.match(renderMarkdown(nested), /data-mermaid-diagram="source"/);
  const listed = ['1. 调用链:', '', '   ```mermaid', '   flowchart TD', '   A --> B', '   ```'].join('\n');
  assert.match(renderMarkdown(listed), /data-mermaid-diagram="source"/);

  const unclosed = ['> ```mermaid', '> flowchart TD', '> A --> B'].join('\n');
  assert.ok(!renderMarkdown(unclosed).includes('data-mermaid-diagram='));
});

// 场景:模型漏写了围栏的语言名,或者把图类型当语言名写(```flowchart、```sequenceDiagram、```mmd)。
// 期望:内容第一行是五种可绘制图类型的开头(前面可以有指令 / front matter)时照样当图画,图框与
// 写了 mermaid 的完全一样;内容不是图的、写了别的语言名(text、markdown)的、五种以外的图类型,
// 仍然是普通代码块。
// 回归:这类围栏以前只显示源码,用户看到的就是一段 `flowchart TD ...` 文字。
await test('fences without the mermaid info string are recognized by their first line', () => {
  const body = 'flowchart TD\nA --> B';
  const fenced = (info, content = body) => renderMarkdown(`\`\`\`${info}\n${content}\n\`\`\``);
  for (const info of ['', 'flowchart', 'Flowchart', 'graph', 'mmd']) {
    assert.match(fenced(info), /data-mermaid-diagram="source"/, info);
    assert.match(fenced(info), /data-code-lang="mermaid"/, info);
  }
  assert.match(fenced('sequenceDiagram', samples.sequence), /data-mermaid-diagram="source"/);
  assert.match(fenced('', `%%{init: {"theme": "dark"}}%%\n${body}`), /data-mermaid-diagram="source"/);

  for (const [info, content] of [
    ['text', body],
    ['markdown', body],
    ['', 'npm install\nflowchart TD'],
    ['', 'pie title 占比\n"A" : 40'],
    ['flowchart', 'not a diagram'],
  ]) {
    assert.ok(!fenced(info, content).includes('data-mermaid-diagram='), `${info}: ${content}`);
  }
});

const tick = () => new Promise((resolve) => setImmediate(resolve));

function rendererFixture({ deferred = false, result = { svg: safeSvg, width: 160, height: 80 } } = {}) {
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
  const stop = installMermaidRenderer(win, doc, (source, theme) => {
    let resolve;
    const pending = new Promise((done) => { resolve = () => done(result); });
    calls.push({ source, theme, resolve });
    if (!deferred) resolve();
    return pending;
  });
  return {
    root, panel, frame, target, calls, events, stop, win,
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
    // 只写原始宽度,高度交给 CSS 按比例算,宽图才能整体缩到对话栏宽。
    assert.deepEqual(page.image().style, { width: '160px' });
    assert.equal(page.image().height, 80);

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

// 场景:图画不出来(这里是第 3 行语法有误);随后源码被改对,重新绘制成功。
// 期望:失败时源码照旧显示,绘制区里多一行说明,框上记下卡在哪一层(data-mermaid-error);
// 恢复成待绘制状态时这两样都清掉。
// 回归:以前失败和「还没画」看起来一模一样,用户和我们都不知道是哪一层出的问题。
await test('a failed diagram shows a one-line note and records the failing layer', async () => {
  const page = rendererFixture({ result: { failure: { stage: 'parse', reason: 'Parse error on line 3', line: 3 } } });
  try {
    await tick();
    assert.equal(page.frame.getAttribute('data-mermaid-diagram'), 'error');
    assert.equal(page.frame.getAttribute('data-mermaid-error'), 'parse: Parse error on line 3');
    assert.equal(page.target.children.length, 1);
    assert.equal(page.target.children[0].className, 'ace-mermaid-error-note');
    assert.equal(page.target.children[0].textContent, '第 3 行语法有误');
    assert.equal(page.target.getAttribute('aria-busy'), 'false');

    // 背景变了会重新绘制:先回到待绘制状态,说明和记录都要清掉。
    page.frame.setAttribute('data-mermaid-diagram', 'ready');
    page.panel.background = '#111111';
    page.mutate(page.panel, 'style');
    assert.equal(page.frame.getAttribute('data-mermaid-error'), null);
    assert.equal(page.target.children.length, 0);
  } finally { page.stop(); }
});

// 场景:mermaid 组件本身没取到(页面开着的时候前端换了新版本,按需加载的文件名已经变了)。
// 期望:说明后面带一个「重新加载」按钮,点了就重新加载页面(桌面版里按 F5 不管用);别的失败不带按钮。
await test('a diagram whose component failed to load offers a reload button', async () => {
  const page = rendererFixture({ result: { failure: { stage: 'load', reason: 'Failed to fetch dynamically imported module', line: 0 } } });
  try {
    await tick();
    const note = page.target.children[0];
    assert.equal(note.textContent, '图表组件加载失败');
    assert.equal(note.children.length, 1);
    assert.equal(note.children[0].className, 'ace-mermaid-error-action');
    assert.equal(note.children[0].textContent, '重新加载');
    let reloads = 0;
    page.win.location = { reload() { reloads += 1; } };
    note.children[0].emit('click');
    assert.equal(reloads, 1);
  } finally { page.stop(); }
});
