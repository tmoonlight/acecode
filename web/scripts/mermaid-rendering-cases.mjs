// test-mermaid-rendering.mjs 用的样例。三组:
// - drawn:模型常见写法,必须画得出来(其中 repaired 为 true 的要靠解析失败后的自动修复)。
// - refused:应当显示源码的,连同期望卡在哪一层。
// - hostile:想让页面在绘制时向外取资源的写法。画不画得出来不限,但页面不许向 HOSTILE_HOST 发请求。

export const HOSTILE_HOST = 'https://attacker.example';
const H = HOSTILE_HOST;

export const drawn = {
  // ---- 闸门以前误拒的 ----
  'class-annotation': 'classDiagram\nclass Shape {\n  <<interface>>\n  +draw() void\n}\nclass Color {\n  <<enumeration>>\n  RED\n}\nShape <|.. Circle',
  'class-annotation-inline': 'classDiagram\nclass Shape\n<<interface>> Shape\nShape : +draw()',
  'state-choice': 'stateDiagram-v2\nstate check <<choice>>\n[*] --> check\ncheck --> A: ok\ncheck --> B: fail',
  'state-fork': 'stateDiagram-v2\nstate f <<fork>>\nstate j <<join>>\n[*] --> f\nf --> A\nf --> B\nA --> j\nB --> j\nj --> [*]',
  'init-directive': "%%{init: {'theme': 'base', 'themeVariables': {'primaryColor': '#ffcc00'}}}%%\nflowchart TD\nA[开始] --> B[结束]",
  'frontmatter-title': '---\ntitle: 订单流程\n---\nflowchart LR\nA --> B',
  'frontmatter-config': '---\nconfig:\n  theme: forest\n---\nflowchart LR\nA --> B',
  'entity-lt': 'flowchart TD\nA["List&lt;String&gt;"] --> B["a &amp; b &quot;c&quot;"]',
  'md-link-shape': 'sequenceDiagram\nA->>B: handlers[i](req)',
  'md-image-shape': 'flowchart TD\nA["if (![]) return"] --> B',
  'math-dollars': 'flowchart TD\nA["price in $$"] --> B["cost $$ again"]',
  'fa-icon': 'flowchart TD\nA[fa:fa-user 用户] --> B[fa:fa-database 数据库]',
  'behavior-word': 'flowchart TD\nA["Expected behavior: retry"] --> B["scroll-behavior: smooth"]',
  'java-annotation': 'classDiagram\nclass AppConfig {\n  @Import(DbConfig.class)\n  @Document(collection="users")\n}',
  'css-media-word': 'flowchart TD\nA["@media (max-width: 600px)"] --> B["@supports grid"]',
  'expression-word': 'flowchart TD\nA["eval expression (a+b)"] --> B',
  'class-member-call': 'classDiagram\nclass Api {\n  call(req) Response\n  callback: Fn\n  link: String\n  href: String\n}',
  'click-statement': 'flowchart TD\nA[文档] --> B[首页]\nclick A "https://example.test" "打开"\nclick B href "https://example.test/b"',
  'click-callback': 'flowchart TD\nA --> B\nclick A callback "tip"\nclick B call fn()',
  'seq-links': 'sequenceDiagram\nparticipant Alice\nlink Alice: Dashboard @ https://example.test\nlinks Alice: {"Wiki": "https://example.test/wiki"}\nAlice->>Bob: hi',
  'graph-no-direction': 'graph\nA --> B',
  'url-in-label': 'flowchart TD\nA["GET https://api.example.test/v1/users"] --> B["file:///C:/tmp/a.txt"]',
  'seq-url': 'sequenceDiagram\nClient->>Server: POST http://localhost:8080/api/login\nServer-->>Client: 302 Location: //cdn.example.test/x',
  'quoted-at-brace': 'flowchart TD\nA["user@{domain}"] --> B',
  'cjk-angle': 'flowchart TD\nA["cache/SCREEN_SHOT/<时间戳>"] --> B["isCore ? CORE : MAIL"]',

  // ---- 本来就该能画的 ----
  'flow-styles': 'flowchart LR\nA:::hot --> B\nclassDef hot fill:#f00,stroke:#333\nstyle B color:#111\nlinkStyle 0 stroke:#00f,stroke-width:2px',
  'flow-subgraph': 'flowchart TD\nsubgraph 客户端[客户端 Client]\nU --> B\nend\nB --> S',
  'flow-cjk-ids': 'flowchart TD\n开始[开始节点] --> 判断{是否通过?}\n判断 -->|是| 结束([完成])',
  'flow-semicolon-label': 'flowchart TD\nA[a = 1; b = 2] --> B',
  'flow-numbered-label': 'flowchart TD\nA["1. 初始化"] --> B["2. 运行"]',
  'seq-blocks': 'sequenceDiagram\nautonumber\nA->>B: req\nalt 成功 (200)\nB-->>A: ok\nelse 失败 [500]\nB-->>A: err\nend\nNote over A,B: 多行<br/>注释',
  'seq-code-text': 'sequenceDiagram\nA->>B: exec("javascript:insert(\'<html>\')") a;b #1\nNote over A: style="width:100%;height:auto"',
  'state-composite': 'stateDiagram-v2\nstate 运行中 {\n  [*] --> 子状态1\n  子状态1 --> 子状态2\n}\n[*] --> 运行中',
  'class-generics': 'classDiagram\nclass Repo {\n  +List<User> findAll()\n  +Map<String, List<Integer>> index\n}',
  'er-attrs': 'erDiagram\nUSER {\n  bigint id PK "主键"\n  varchar(64) name\n}\nUSER ||--o{ ORDER : "下单"',

  // ---- 语法小错,靠自动修复 ----
  'fix/paren-in-label': 'flowchart TD\nA[调用 init() 方法] --> B[返回结果 (成功)]',
  'fix/paren-in-round': 'flowchart TD\nA(调用 foo(bar)) --> B',
  'fix/bracket-in-label': 'flowchart TD\nA[数组 arr[0] 取值] --> B[map[key]]',
  'fix/brace-in-label': 'flowchart TD\nA[返回 {code: 0}] --> B{是否 {ok}?}',
  'fix/pipe-in-label': 'flowchart TD\nA[a | b] --> B[x || y]',
  'fix/quote-in-label': 'flowchart TD\nA[打印 "hello"] --> B',
  'fix/quote-in-quoted': 'flowchart TD\nA["他说 "你好" 然后离开"] --> B',
  'fix/slash-start': 'flowchart TD\nA[/api/users 接口] --> B[/etc/hosts]',
  'fix/edge-label-paren': 'flowchart TD\nA -->|调用 f(x)| B\nB -- 返回 (ok) --> C',
  'fix/id-end': 'flowchart TD\nstart[开始] --> end[结束]',
  'fix/id-end-plain': 'flowchart TD\nA --> end\nend --> B',
  'fix/id-default': 'flowchart TD\ndefault[默认] --> style[样式]\nstyle --> class[类]',
  'fix/id-graph': 'flowchart TD\ngraph[图] --> subgraph1[子图]',
  'fix/id-call-click': 'flowchart TD\ncall[调用接口] --> callback[回调处理]\nclick[点击按钮] --> call\nlink[跳转] --> click',
  'fix/subgraph-title': 'flowchart TD\nsubgraph 客户端 (Client)\nA --> B\nend\nsubgraph 服务 端\nC --> D\nend',
  'fix/at-in-label': 'flowchart TD\nA[user@example.com] --> B[@Autowired]',
  'fix/inline-comment': 'flowchart TD\nA --> B %% 注释\nB --> C',
  'fix/empty-label': 'flowchart TD\nA[] --> B[ ]',
  'fix/br-then-paren': 'flowchart TD\nGN1[用户数据生成<br/>(语法错误)] --> B',
  'fix/seq-activation': 'sequenceDiagram\nA->>+B: 请求\nB-->>-A: 响应\nB-->>-A: 再响应',
  'fix/seq-unclosed-block': 'sequenceDiagram\nloop 重试\nA->>B: x',
  'fix/seq-extra-end': 'sequenceDiagram\nA->>B: x\nend',
  'fix/state-hyphen': 'stateDiagram-v2\n[*] --> pre-check\npre-check --> post-check',
  'fix/class-hyphen': 'classDiagram\nclass my-class\nmy-class --> other-class',
  'fix/class-scope': 'classDiagram\nclass acecode::AgentLoop {\n  +run() void\n}\nclass std::string',
  'fix/class-brace-arg': 'classDiagram\nclass A {\n  +f(x = 1, y = "s") void\n  +g(opts: {a: number}) void\n}',
  'fix/er-no-label': 'erDiagram\nA ||--o{ B',
};

export const repaired = new Set(Object.keys(drawn).filter((id) => id.startsWith('fix/')));

// 「画出来了」不等于「画对了」:图上必须出现 / 不许出现的文字(比较时忽略空白)。
export const shows = {
  'class-annotation': ['«interface»', '«enumeration»', '+draw()'],
  'entity-lt': ['List<String>', 'a & b "c"'],
  'cjk-angle': ['<时间戳>', 'isCore ? CORE : MAIL'],
  'class-generics': ['List<User>'],
  'seq-code-text': ['a;b #1', 'width:100%;height:auto'],
  'click-statement': ['文档', '首页'],
  'fix/paren-in-label': ['调用 init() 方法', '返回结果 (成功)'],
  'fix/paren-in-round': ['调用 foo(bar)'],
  'fix/bracket-in-label': ['数组 arr[0] 取值', 'map[key]'],
  'fix/brace-in-label': ['返回 {code: 0}', '是否 {ok}?'],
  'fix/pipe-in-label': ['a | b', 'x || y'],
  'fix/quote-in-label': ['打印 "hello"'],
  'fix/quote-in-quoted': ['他说 "你好" 然后离开'],
  'fix/slash-start': ['/api/users 接口', '/etc/hosts'],
  'fix/edge-label-paren': ['调用 f(x)', '返回 (ok)'],
  'fix/id-end': ['开始', '结束'],
  'fix/id-end-plain': ['end'],
  'fix/id-default': ['默认', '样式', '类'],
  'fix/id-graph': ['图', '子图'],
  'fix/id-call-click': ['调用接口', '回调处理', '点击按钮', '跳转'],
  'fix/subgraph-title': ['客户端 (Client)', '服务 端'],
  'fix/at-in-label': ['user@example.com', '@Autowired'],
  'fix/br-then-paren': ['用户数据生成', '(语法错误)'],
  'fix/seq-activation': ['请求', '响应', '再响应'],
  'fix/seq-unclosed-block': ['重试'],
  'fix/state-hyphen': ['pre-check', 'post-check'],
  'fix/class-hyphen': ['my-class', 'other-class'],
  'fix/class-scope': ['acecode::AgentLoop', 'std::string', '+run()'],
  'fix/class-brace-arg': ['{a: number}'],
};
export const hides = {
  'fix/id-end': ['end'],
  'fix/id-default': ['default', 'style', 'class'],
  'fix/id-call-click': ['call', 'click', 'link'],
  'fix/quote-in-label': ['quot'],
  'fix/class-brace-arg': ['#123', '&#'],
  'entity-lt': ['&lt;', 'quot'],
};

export const refused = {
  'pie': ['pie title 占比\n"A" : 40\n"B" : 60', 'gate', 'unsupported-family'],
  'leading-text': ['下面是流程图:\nflowchart TD\nA --> B', 'gate', 'unsupported-family'],
  'style-url': ['flowchart TD\nA --> B\nstyle A fill:url(#grad),stroke:#333', 'gate', 'external-resource'],
  'shape-block': ['flowchart TD\nA@{ shape: rounded, label: "开始" } --> B', 'gate', 'generalized-shape'],
  'state-named-click': ['stateDiagram-v2\n[*] --> click\nclick --> call: 触发', 'parse', ''],
  'seq-participant-link': ['sequenceDiagram\nparticipant link\nlink->>call: go', 'parse', ''],
  'id-with-space': ['flowchart TD\nstep one --> step two', 'parse', ''],
};

const css = (name) => `mask-image:url(${H}/${name}.png),background-image:url(${H}/${name}b.png),filter:url(${H}/${name}.svg#f)`;
const icon = (name) => `{"icon": "${H}/${name}.png"}`;

export const hostile = {
  // 时序图参与者图标:绘制时当场取图。各种把 properties 接进语句里的写法。
  'seq-properties': `sequenceDiagram\nparticipant A\nproperties A: ${icon('p1')}\nA->>B: hi`,
  'seq-properties-upper': `sequenceDiagram\nparticipant A\nPROPERTIES A: ${icon('p2')}\nA->>B: hi`,
  'seq-properties-semicolon': `sequenceDiagram\nparticipant A; properties A: ${icon('p3')}\nA->>B: hi`,
  'seq-properties-after-end': `sequenceDiagram\nparticipant A\nloop x\nA->>B: hi\nend properties A: ${icon('p4')}`,
  'seq-properties-after-box-end': `sequenceDiagram\nbox T\nparticipant A\nend properties A: ${icon('p5')}\nA->>B: hi`,
  'seq-properties-after-accdescr': `sequenceDiagram\nparticipant A\naccDescr {\n x\n} properties A: ${icon('p6')}\nA->>B: hi`,
  'seq-properties-two-lines': `sequenceDiagram\nparticipant A\nproperties \nA: ${icon('p7')}\nA->>B: hi`,
  'seq-properties-nbsp': `sequenceDiagram\nparticipant A\n\u00a0properties A: ${icon('p8')}\nA->>B: hi`,
  'seq-details': `sequenceDiagram\nparticipant A\ndetails A: ${icon('p9')}\nA->>B: hi`,
  'seq-links': `sequenceDiagram\nparticipant A\nlinks A: {"a": "${H}/l1"}\nlink A: b @ ${H}/l2\nA->>B: hi`,
  'seq-typed-participant': `sequenceDiagram\nparticipant A@{ "type": "database", "icon": "${H}/tp1.png" }\nA->>B: x`,
  'seq-note-img': `sequenceDiagram\nA->>B: <img src=${H}/sq1.png>\nNote over A: <svg><image href="${H}/sq2.png"/></svg>`,
  'seq-rect-url': `sequenceDiagram\nrect url(${H}/r1.svg#p)\nA->>B: x\nend`,
  // 流程图:图片形状、标签里的标签、各种写法的实体、样式里的外链、指令和 front matter。
  'flow-img-shape': `flowchart TD\nA@{ img: "${H}/i1.png", label: "x", w: 60, h: 60 } --> B`,
  'flow-img-shape-escaped-key': `flowchart TD\nA@{ "\\u0069mg": "${H}/i2.png" } --> B`,
  'flow-img-shape-behind-quote': `flowchart TD\nA["a] --> B@{ img: "${H}/i3.png" } --> C[b"]`,
  'flow-img-tag': `flowchart TD\nA["<img src='${H}/t1.png'>"] --> B`,
  'flow-img-entity': `flowchart TD\nA["&lt;img src='${H}/t2.png'&gt;"] --> B["&#60;img src='${H}/t3.png'&#62;"]`,
  'flow-img-mermaid-entity': `flowchart TD\nA["#lt;img src='${H}/t4.png'#gt;"] --> B["#60;img src='${H}/t5.png'#62;"]`,
  'flow-iframe': `flowchart TD\nA["<iframe src=${H}/f1></iframe><svg><image href=${H}/f2.png /></svg>"] --> B`,
  'flow-md-image': `flowchart TD\nA["\`![x](${H}/m1.png)\`"] --> B["[y](${H}/m2)"]`,
  'flow-stereotype-smuggle': `flowchart TD\nA["<<img src=${H}/g1.png>>"] --> B["<<script>>alert(1)<</script>>"]`,
  'flow-style-url': `flowchart TD\nA --> B\nstyle A ${css('s1')}`,
  'flow-style-after-semicolon': `flowchart TD\nA --> B; style A fill:url(${H}/s2.svg#p)`,
  'flow-style-after-end': `flowchart TD\nsubgraph S\nA --> B\nend style A ${css('s3')}`,
  'flow-classdef-default': `flowchart TD\nA --> B\nclassDef default ${css('s4')}`,
  'flow-click': `flowchart TD\nA --> B\nclick A href "${H}/c1" "tip"\nclick B call alert(1)`,
  'flow-click-after-semicolon': `flowchart TD\nA --> B; click A href "${H}/c2"`,
  'flow-init-themecss': `%%{init: {"themeCSS": ".node rect { fill: url(${H}/x1.svg#p); background-image: url(${H}/x2.png) }"}}%%\nflowchart TD\nA --> B`,
  'flow-init-mid-diagram': `flowchart TD\n%%{init: {"themeCSS": "g { background-image: url(${H}/x3.png) }"}}%%\nA --> B`,
  'flow-init-unclosed': `%%{init: {"securityLevel": "loose", "themeCSS": "g { background: url(${H}/x4.png) }"\nflowchart TD\nA --> B`,
  'flow-frontmatter-css': `---\nconfig:\n  themeCSS: ".node rect { fill: url(${H}/x5.svg#p) }"\n  securityLevel: loose\n---\nflowchart TD\nA["<img src='${H}/x6.png'>"] --> B`,
  // 其它图类型。
  'class-stereotype-smuggle': `classDiagram\nclass A {\n  <<img src=${H}/g2.png>>\n  <<style>>*{background:url(${H}/g3.png)}<</style>>\n}`,
  'class-link': `classDiagram\nclass A\nlink A "${H}/cl1" "tip"\nclick A href "${H}/cl2"\ncallback A "alert" "tip"`,
  'class-note-img': `classDiagram\nclass A\nnote for A "<img src=${H}/n1.png>"`,
  'class-cssclass': `classDiagram\nclass A\nclassDef c ${css('cs1')}\ncssClass "A" c`,
  'state-note-img': `stateDiagram-v2\nA --> B\nnote right of A\n  <img src=${H}/sn1.png>\nend note`,
  'state-style-after-brace': `stateDiagram-v2\nstate S {\n A --> B\n} style A ${css('ss1')}`,
  'state-classdef': `stateDiagram-v2\nclassDef c ${css('ss2')}\nA:::c --> B`,
  'er-style-url': `erDiagram\nA ||--o{ B : x\nstyle A fill:url(${H}/e1.svg#p)`,
};

// 端到端(真实的 Markdown → 图框 → 渲染器):[Markdown, 期望终态, 期望的 data-mermaid-error 开头]。
const fence = (source, indent = '') => [`${indent}\`\`\`mermaid`, ...source.split('\n').map((line) => indent + line), `${indent}\`\`\``].join('\n');
export const pages = {
  'plain': [fence(drawn['cjk-angle']), 'ready', ''],
  'in-list': [`1. 调用链如下:\n\n${fence(drawn['flow-subgraph'], '   ')}\n\n2. 结束`, 'ready', ''],
  'in-quote': [fence(drawn['seq-blocks']).split('\n').map((line) => `> ${line}`).join('\n'), 'ready', ''],
  'repaired': [fence(drawn['fix/paren-in-label']), 'ready', ''],
  'bare-fence': [`\`\`\`\n${drawn['cjk-angle']}\n\`\`\``, 'ready', ''],
  'type-as-info': [`\`\`\`sequenceDiagram\n${drawn['seq-blocks']}\n\`\`\``, 'ready', ''],
  'syntax-error': [fence('flowchart TD\nA --> B\nstep one --> step two'), 'error', 'parse'],
  'unsupported': [fence(refused.pie[0]), 'error', 'gate: unsupported-family'],
};
export const pageNotes = { 'syntax-error': '第 3 行语法有误', 'unsupported': '暂不支持这种图表类型' };
