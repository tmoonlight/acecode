import assert from 'node:assert/strict';

import {
  dropMermaidInteractions,
  inspectMermaidModel,
  inspectMermaidSource,
  mermaidErrorLine,
  mermaidFamilyOf,
  neutralizeMermaidMarkup,
  normalizeMermaidEntities,
  prepareMermaidRenderSource,
  stripMermaidPreamble,
} from './mermaidSource.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const prepared = (source) => {
  const inspected = inspectMermaidSource(source);
  assert.equal(inspected.ok, true, `${source}\n-> ${inspected.reason}`);
  return prepareMermaidRenderSource(inspected.source, inspected.family);
};

// 场景:用户反馈「图没画出来」的那张调用链流程图(类名已换成通用写法)。标签全部带引号,里面有
// `<br/>`、`<时间戳>` 这种尖括号包中文、三目运算符的 `? :`、单引号和加号。
// 期望:闸门放行,而且交给 Mermaid 的文本与原文一字不差(合法的图不改写)。
test('reported quoted call-chain flowchart passes the gate untouched', () => {
  const source = [
    'flowchart TD',
    '    A["悬浮按钮/长按标题栏截屏<br/>ScreenUtil.gotoForward"] --> B["截屏加密存盘<br/>cache/LONG_CLICK_SCREEN_SHOT/<时间戳>"]',
    '    B --> C["RouterUtils.jump2ForwardActivity<br/>底部转发弹层"]',
    '    C --> D["点击「转给邮件」<br/>buildForwardMenuItem"]',
    '    D --> E["WriteBean.setOtherJson<br/>type=FromScreenShot, value=裸文件名"]',
    '    E --> F["RouterUtils.jump2WriteActivity<br/>isCore ? CORE_WRITE : MAIL_WRITE"]',
    '    F --> G["SendActivity.getContactsByKey"]',
    "    G --> H[\"attachList.add<br/>createAttachBean('FromScreenShot:'+name)\"]",
    '    H --> I["附件列表显示「来自应用的截屏.jpg」<br/>上传时解密转 base64 上传"]',
  ].join('\n');
  assert.deepEqual(inspectMermaidSource(source), { ok: true, family: 'flowchart', source });
  assert.equal(prepareMermaidRenderSource(source, 'flowchart'), source);
});

// 场景:五种图的开头写法。`graph` / `flowchart` 不写方向时 Mermaid 按从上到下画。
// 期望:认得出五种;其它图类型、开头夹着说明文字的,仍然报不支持。
test('family detection accepts the five families and a direction-less flowchart header', () => {
  assert.equal(mermaidFamilyOf('graph\nA --> B'), 'flowchart');
  assert.equal(mermaidFamilyOf('%% 注释\n\nflowchart LR;\nA --> B'), 'flowchart');
  assert.equal(mermaidFamilyOf('stateDiagram\n[*] --> A'), 'state');
  assert.equal(mermaidFamilyOf('pie\ntitle x'), '');
  assert.equal(inspectMermaidSource('下面是流程图:\nflowchart TD\nA --> B').reason, 'unsupported-family');
});

// 场景:模型在图前面写了 `%%{init: ...}%%` 指令或 `---` front matter 来改主题 / 加标题。
// 期望:不再整张退回源码;交给 Mermaid 的文本里这两样被空行替换(行数不变,报错行号才对得上),
// 一个 `%%{` 都不剩。指令可以出现在图中间,也可以没有收尾;拿掉一段后剩下的字符又拼出一段的,
// 要继续拿到干净为止。front matter 没有收尾的无法判断到哪结束,仍然拒绝。
// 回归:以前带指令的图报「不支持的图类型」,带 front matter 的报 frontmatter,全部显示成源码。
test('directives and front matter are blanked out instead of rejecting the diagram', () => {
  const directive = "%%{init: {'theme': 'base', 'themeVariables': {'primaryColor': '#ffcc00'}}}%%\nflowchart TD\nA[开始] --> B[结束]";
  assert.equal(prepared(directive), '\nflowchart TD\nA[开始] --> B[结束]');

  const front = '---\ntitle: 订单流程\nconfig:\n  theme: forest\n---\nflowchart LR\nA --> B';
  assert.equal(prepared(front), '\n\n\n\n\nflowchart LR\nA --> B');

  const multiline = 'flowchart TD\n%%{init: {\n  "theme": "dark"\n}}%%\nA --> B';
  assert.equal(prepared(multiline), 'flowchart TD\n\n\n\nA --> B');

  for (const source of [
    'flowchart TD\nA --> B %%{init: {"theme":"dark"}',
    '%%%%{a}%%{init: {"securityLevel": "loose"}}%%\nflowchart TD\nA --> B',
    '%%{init: {}}%%\n---\nconfig:\n  securityLevel: loose\n---\nflowchart TD\nA --> B',
  ]) {
    const text = prepared(source);
    assert.equal(/%%\s*\{/.test(text), false, source);
    assert.equal(/securityLevel/.test(text), false, source);
    assert.equal(text.split('\n').length, source.split('\n').length, source);
  }

  assert.equal(stripMermaidPreamble('---\ntitle: x\nflowchart TD\nA --> B'), null);
  assert.equal(inspectMermaidSource('---\ntitle: x\nflowchart TD\nA --> B').reason, 'frontmatter');
  assert.equal(inspectMermaidSource('%% 说明\n---\ntitle: x\n---\nflowchart TD').reason, 'frontmatter');
});

// 场景:标签里写了 HTML 实体(`&lt;`、`&#60;`、`&#x3c;`、`&amp;`、`&quot;`)。
// 期望:换成 Mermaid 自己的写法 `#lt;` `#60;` `#amp;`(十六进制换成十进制,Mermaid 只认十进制),
// 与直接写尖括号得到同一个结果;时序图消息里换出来的实体码不会再被当成 `#` 注释转义一遍。
// 回归:以前只要出现 `&lt;` / `&gt;` 整张图就被判成 html 拒绝。
test('HTML entities become Mermaid entity codes', () => {
  assert.equal(
    normalizeMermaidEntities('A["List&lt;String&gt; &amp; x &#60;T&#62; &#x3C;U&#x3e; &quot;q&quot;"]'),
    'A["List#lt;String#gt; #amp; x #60;T#62; #60;U#62; #quot;q#quot;"]',
  );
  assert.equal(normalizeMermaidEntities('A & B --> C'), 'A & B --> C');
  assert.equal(
    prepared('flowchart TD\nA["&lt;img src=x onerror=alert(1)&gt;"] --> B'),
    'flowchart TD\nA["#lt;img src=x onerror=alert(1)#gt;"] --> B',
  );
  assert.equal(
    prepared('sequenceDiagram\nA->>B: Map&lt;K,V&gt; a;b'),
    'sequenceDiagram\nA->>B: Map#lt;K,V#gt; a#59;b',
  );
});

// 场景:类图的 `<<interface>>` / `<<enumeration>>` 标记,状态图的 `<<choice>>` / `<<fork>>` / `<<join>>`。
// 期望:整行只有这个标记(类图后面最多跟一个类名)时原样保留;别的图类型、标记后面还夹着内容、
// 名字里带属性的,照旧改写成实体码,闸门的「不能有标签」自检也不放过。
// 回归:尖括号改写把 `<<interface>>` 改成了 `<#lt;interface#gt;>`,类图里画成一行普通成员,状态图的
// 判断菱形画成了普通状态,`<<interface>> Shape` 的写法直接语法错误。
test('class and state stereotypes survive markup neutralization', () => {
  const classSource = 'classDiagram\nclass Shape {\n  <<interface>>\n  +draw() void\n}\n<<enumeration>> Color\nShape <|.. Circle';
  assert.equal(prepared(classSource), classSource);
  const stateSource = 'stateDiagram-v2\nstate check <<choice>>\nstate f <<fork>>\n[*] --> check';
  assert.equal(prepared(stateSource), stateSource);

  assert.equal(
    neutralizeMermaidMarkup('A["<<interface>>"] --> B', 'flowchart'),
    'A["<#lt;interface#gt;>"] --> B',
  );
  for (const [line, family] of [
    ['  <<img src=x>>', 'class'],
    ['  <<style>>*{color:red}<</style>>', 'class'],
    ['  <<interface>> Shape <script>', 'class'],
    ['state x <<choice>> <b>', 'state'],
    ['state x <<iframe>>', 'state'],
  ]) {
    const neutral = neutralizeMermaidMarkup(line, family);
    assert.equal(/<\/?[A-Za-z]/.test(neutral), false, line);
  }
});

// 场景:讲代码的图在标签里出现了像「危险写法」的普通文字:`behavior: retry`、Java 注解 `@Import` /
// `@Document`、`@media (...)`、`eval expression (a+b)`、`handlers[i](req)`、`if (![]) return`、`$$`、
// `fa:fa-user`、地址和协议名;节点名叫 call / callback / click / link,类成员叫 call() / link。
// 期望:全部放行,交给 Mermaid 的文本不变。这些检查只该看样式语句和交互语句,不该看标签文字。
// 回归:以上每一条以前都会让整张图退回源码(unsafe-style / markdown-link / math-label / icon-label /
// interactive)。
test('label text that merely looks risky is no longer rejected', () => {
  for (const source of [
    'flowchart TD\nA["Expected behavior: retry"] --> B["scroll-behavior: smooth"]',
    'classDiagram\nclass AppConfig {\n  @Import(DbConfig.class)\n  @Document(collection="users")\n}',
    'flowchart TD\nA["@media (max-width: 600px)"] --> B["@supports grid"]',
    'flowchart TD\nA["eval expression (a+b)"] --> B',
    'sequenceDiagram\nA->>B: handlers[i](req)',
    'flowchart TD\nA["if (![]) return"] --> B',
    'flowchart TD\nA["price in $$"] --> B["cost $$ again"]',
    'flowchart TD\nA[fa:fa-user 用户] --> B',
    'flowchart TD\nA["GET https://api.example.test/v1/users"] --> B["file:///C:/tmp/a.txt"]',
    'flowchart TD\ncall[调用接口] --> callback[回调处理]\nclick[点击按钮] --> call\nlink[跳转] --> click',
    'classDiagram\nclass Api {\n  call(req) Response\n  callback: Fn\n  link: String\n  click handler\n}',
    'flowchart TD\nA["user@{domain}"] --> B',
  ]) {
    assert.equal(prepared(source), source);
  }
});

// 场景:图里带点击 / 链接 / 参与者属性语句。
// 期望:常见写法整句换成空行(图照画,只是没有链接);类图成员区里同名的成员不动;把关键字当节点名、
// 状态名、参与者名用的行不动(交给 Mermaid 报错,不悄悄丢一条连线或消息)。
// 回归:以前只要有一行 click / link 整张图退回源码;反过来,状态图里叫 click 的状态也被误拒。
test('interaction statements are dropped, same-named identifiers are kept', () => {
  assert.equal(
    prepared('flowchart TD\nA[文档] --> B[首页]\nclick A "https://example.test" "打开"\n  click B href "https://example.test/b"\nB --> C'),
    'flowchart TD\nA[文档] --> B[首页]\n\n\nB --> C',
  );
  assert.equal(
    prepared('classDiagram\nclass A {\n  link String\n  click handler\n}\nlink A "https://example.test" "tip"\ncallback A "fn" "tip"\nclick A href "https://example.test"\nA --> B'),
    'classDiagram\nclass A {\n  link String\n  click handler\n}\n\n\n\nA --> B',
  );
  assert.equal(
    prepared('sequenceDiagram\nparticipant Alice\nlink Alice: Dashboard @ https://example.test\nLINKS Alice: {"Wiki": "https://example.test/wiki"}\n  properties Alice: {"class": "x"}\ndetails Alice: x\nAlice->>Bob: hi'),
    'sequenceDiagram\nparticipant Alice\n\n\n\n\nAlice->>Bob: hi',
  );
  // 时序图里 `;` 等于换行:接在别的语句后面的也要拿掉,消息文字里的 `;` 不受影响。
  assert.equal(
    dropMermaidInteractions('participant A; properties A: {"icon": "x"}; activate A', 'sequence'),
    'participant A; activate A',
  );
  assert.equal(
    prepared('sequenceDiagram\nA->>B: see details; link below\nNote over A: properties: none'),
    'sequenceDiagram\nA->>B: see details#59; link below\nNote over A: properties: none',
  );

  for (const source of [
    'stateDiagram-v2\n[*] --> click\nclick --> call: 触发',
    'sequenceDiagram\nparticipant link\nlink->>call: go',
    'sequenceDiagram\nparticipant D as Order details page\nD->>P: load properties',
  ]) {
    assert.equal(prepared(source), source);
  }
});

// 场景:拿不掉、只能拒绝的写法。样式语句(行首或 `;` 之后的 style / classDef / linkStyle / cssClass,
// 时序图的 rect / box)里的 url() 与老式脚本化 CSS;`@{ ... }` 新式形状(里面能写图片地址)。
// 期望:拒绝并给出原因。流程图里引号内的 `@{` 是标签文字,不算。
test('style resources and shape blocks are still rejected at the gate', () => {
  for (const source of [
    'flowchart TD\nA --> B\nstyle A fill:url(#grad)',
    'flowchart TD\nA --> B; style A fill:url(https://example.test/x.svg#p)',
    'flowchart TD\nA:::c --> B\n  classDef c fill:URL ("x.svg")',
    'classDiagram\nclass A\ncssClass "A" url(x)',
    'sequenceDiagram\nrect url(https://example.test/r.svg#p)\nA->>B: x\nend',
    'sequenceDiagram\nbox url(x) T\nparticipant A\nend',
  ]) {
    assert.equal(inspectMermaidSource(source).reason, 'external-resource', source);
  }
  for (const source of [
    'flowchart TD\nA --> B\nstyle A width:expression(alert(1))',
    'flowchart TD\nA --> B; classDef hot @import "theme.css"',
    'stateDiagram-v2\nA --> B\nstyle A behavior:url',
  ]) {
    assert.equal(inspectMermaidSource(source).reason, 'unsafe-style', source);
  }
  for (const source of [
    'flowchart TD\nA@{ shape: rounded, label: "开始" } --> B',
    'flowchart TD\nA@{\n  img: "https://example.test/i.png"\n}\nA --> B',
    'flowchart TD\nA["x"]@{ "\\u0069mg": "https://example.test/i.png" } --> B',
    'sequenceDiagram\nparticipant A@{ "type": "database" }\nA->>B: x',
    'sequenceDiagram\nA->>B: "x @{ y }"',
  ]) {
    assert.equal(inspectMermaidSource(source).reason, 'generalized-shape', source);
  }
});

// 场景:绘制前检查 Mermaid 解析出来的图模型。源码层的检查认的是写法,同一行里换个接法(时序图的
// `end properties A: {"icon": ...}`)就认不出来,而图片地址会在绘制时当场发请求。
// 期望:模型里任何 img 有值、任何 properties 带 icon、任何名字带 style 的字段下面出现 url() / @import /
// expression() 都判为外部资源;标签文字里的 `getUrl()`、参与者的 class 属性不受影响;带环的对象
// 不会转圈,大得离谱的模型直接拒绝。
// 回归:文字层只按「语句开头」丢 properties,`end properties ...` 漏过去,页面向外发了取图请求。
test('parsed model inspection catches resources however they were written', () => {
  const actor = (properties) => ({ state: { records: { actors: new Map([['A', { name: 'A', properties, links: {} }]]) } } });
  assert.equal(inspectMermaidModel(actor({ icon: 'https://example.test/p.png' })), 'external-resource');
  assert.equal(inspectMermaidModel(actor({ icon: '@clock' })), 'external-resource');
  assert.equal(inspectMermaidModel(actor({ class: 'hot' })), '');
  assert.equal(inspectMermaidModel(actor(undefined)), '');

  const flow = (vertex, classes = new Map()) => ({ vertices: new Map([['A', { id: 'A', text: 'getUrl() url(x) @import', styles: [], ...vertex }]]), classes });
  assert.equal(inspectMermaidModel(flow({})), '');
  assert.equal(inspectMermaidModel(flow({ img: 'https://example.test/i.png' })), 'external-resource');
  assert.equal(inspectMermaidModel(flow({ img: '  ' })), '');
  assert.equal(inspectMermaidModel(flow({ styles: ['fill:#f9f', 'mask-image:URL (https://example.test/m.png)'] })), 'external-resource');
  assert.equal(inspectMermaidModel(flow({ labelStyle: 'filter:url(#f)' })), 'external-resource');
  assert.equal(inspectMermaidModel(flow({}, new Map([['hot', { styles: ['fill:#f00'], textStyles: ['@import "x.css"'] }]]))), 'external-resource');
  assert.equal(inspectMermaidModel(flow({}, new Map([['hot', { styles: ['fill:#f00', 'stroke:#333'] }]]))), '');

  const loop = { name: 'x', styles: ['fill:#fff'] };
  loop.self = loop;
  loop.set = new Set([loop]);
  assert.equal(inspectMermaidModel(loop), '');
  assert.equal(inspectMermaidModel(null), '');
  assert.equal(inspectMermaidModel(Array.from({ length: 200001 }, () => ({}))), 'too-large');
});

// 场景:Mermaid 报「第 N 行」时,N 是它自己清理之后的行号:注释行连同前面的空行整段删掉,开头的
// 空行去掉,流程图里以 `}` 结尾的行后面的空行并掉。
// 期望:按同样的规则把 N 对回交给它的文本的行号(与用户看到的源码行号一致);超出范围返回 0。
// 回归:front matter 换成空行后,不换算的话报错行号比源码里的实际行号小。
test('reported parse error lines map back to source lines', () => {
  const plain = 'flowchart TD\nA --> B\nB --> C(';
  assert.equal(mermaidErrorLine(plain, 3, 'flowchart'), 3);

  const afterFrontMatter = '\n\n\nflowchart TD\nA --> B\nB --> C(';
  assert.equal(mermaidErrorLine(afterFrontMatter, 1, 'flowchart'), 4);
  assert.equal(mermaidErrorLine(afterFrontMatter, 3, 'flowchart'), 6);

  const comments = 'sequenceDiagram\n%% 说明\nA->>B: x\n\n\n  %% 再说明\nB->>A: y\n\nA->>B: z';
  assert.equal(mermaidErrorLine(comments, 2, 'sequence'), 3);
  assert.equal(mermaidErrorLine(comments, 3, 'sequence'), 7);
  assert.equal(mermaidErrorLine(comments, 4, 'sequence'), 8);
  assert.equal(mermaidErrorLine(comments, 5, 'sequence'), 9);

  const braces = 'flowchart TD\nA{判断}\n\n\nB --> C';
  assert.equal(mermaidErrorLine(braces, 3, 'flowchart'), 5);
  assert.equal(mermaidErrorLine(braces, 3, 'state'), 3);

  assert.equal(mermaidErrorLine(plain, 4, 'flowchart'), 0);
  assert.equal(mermaidErrorLine(plain, 0, 'flowchart'), 0);
  assert.equal(mermaidErrorLine(plain, Number.NaN, 'flowchart'), 0);
});
