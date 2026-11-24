import assert from 'node:assert/strict';
import mermaid from 'mermaid';

import { mermaidConfig } from './mermaidRenderer.js';
import { repairMermaidSource } from './mermaidRepair.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const flow = (...lines) => ['flowchart TD', ...lines].join('\n');
const repairedFlow = (...lines) => repairMermaidSource(flow(...lines), 'flowchart').split('\n').slice(1);

// 场景:模型最常见的语法小错,节点标签没加引号却带了括号、方括号、花括号、竖线、引号、`@`、开头的
// 斜杠(会被当成平行四边形的开头)。官方解析器在这些地方直接报错,整张图画不出来。
// 期望:只在解析失败后调用的修复,把每个节点的标签包进引号;标签里自带的引号换成 `#quot;`;形状
// (方框、圆角、菱形、圆形、体育场、圆柱、平行四边形)保持不变;闭合符号取「后面跟着连线或行尾」
// 的那一个,标签里自己带的同类符号不会把标签截断。
// 回归:本机会话记录里的 29 张图有 3 张因此显示成源码(`[用户数据生成<br/>(语法错误)]`、`[数字\|字符]`)。
await test('flowchart labels with special characters get quoted', () => {
  assert.deepEqual(repairedFlow(
    'A[调用 init() 方法] --> B[返回结果 (成功)]',
    'C(调用 foo(bar)) --> D{是否 {ok}?}',
    'E[数组 arr[0] 取值] --> F[map[key]]',
    'G[a | b] --> H[user@example.com]',
    'I[打印 "hello"] --> J["他说 "你好" 然后离开"]',
    'K[/api/users 接口] --> L[/真的平行四边形/]',
    'M((圆)) --> N([体育场 (x)])',
    'O[(库 db)] --> P((a) + (b))',
    'Q[] --> R["已经 (带引号)"]',
  ), [
    'A["调用 init() 方法"] --> B["返回结果 (成功)"]',
    'C("调用 foo(bar)") --> D{"是否 {ok}?"}',
    'E["数组 arr[0] 取值"] --> F["map[key]"]',
    'G["a | b"] --> H["user@example.com"]',
    'I["打印 #quot;hello#quot;"] --> J["他说 #quot;你好#quot; 然后离开"]',
    'K["/api/users 接口"] --> L[/"真的平行四边形"/]',
    'M(("圆")) --> N(["体育场 (x)"])',
    'O[("库 db")] --> P("(a) + (b)")',
    'Q[" "] --> R["已经 (带引号)"]',
  ]);
});

// 场景:连线上的文字带括号。`-->|文字|` 与 `-- 文字 -->` 两种写法都解析不了。
// 期望:竖线写法给文字加引号;夹在连线中间的写法换成竖线写法(虚线、粗线各自保持线型)。
await test('flowchart edge labels get quoted in both spellings', () => {
  assert.deepEqual(repairedFlow(
    'A -->|调用 f(x)| B',
    'B -- 返回 (ok) --> C',
    'C -. 重试 [3 次] .-> D',
    'D == 强制 {x} ==> E',
    'E --- F',
    'F -->|"已有引号"| G',
  ), [
    'A -->|"调用 f(x)"| B',
    'B -->|"返回 (ok)"| C',
    'C -.->|"重试 [3 次]"| D',
    'D ==>|"强制 {x}"| E',
    'E --- F',
    'F -->|"已有引号"| G',
  ]);
});

// 场景:节点名正好是 Mermaid 的关键字(最常见的是 end),或者语句后面跟了行内注释。
// 期望:当节点名用的关键字改名(加下划线);单独一行的 end 仍是子图结束;真正的 style / classDef /
// class / linkStyle / direction 语句原样不动,`:::样式类` 后缀不动;语句后面的 `%% 注释` 去掉
// (Mermaid 只认整行注释)。
// 显示文字:这个名字在图里任何一处带过标签(哪怕写在后面),别处的裸引用只改名;整张图里从没带过
// 标签的,才把原名补成显示文字。
// 回归:裸引用一律补 `["原名"]`,Mermaid 里后写的标签盖掉先写的,`style[样式]` 被画成了 "style"。
await test('flowchart reserved-word node ids are renamed, real statements are kept', () => {
  assert.deepEqual(repairedFlow(
    'click --> call',
    'start[开始] --> end[结束]',
    'A --> end',
    'default[默认] --> style[样式]',
    'style --> class[类]',
    'click[点击按钮] --> graph',
    'graph[图] --> B:::default',
    'subgraph S1',
    'direction LR',
    'A --> B %% 行内注释',
    'end',
    'style A fill:#f9f,stroke:#333',
    'classDef default fill:#fff',
    'class A,B hot',
    'linkStyle default stroke:#00f',
    'A --> B; style B fill:#ff0',
    '%% 整行注释 end[x]',
  ), [
    'click_ --> call_["call"]',
    'start["开始"] --> end_["结束"]',
    'A --> end_',
    'default_["默认"] --> style_["样式"]',
    'style_ --> class_["类"]',
    'click_["点击按钮"] --> graph_',
    'graph_["图"] --> B:::default',
    'subgraph S1',
    'direction LR',
    'A --> B',
    'end',
    'style A fill:#f9f,stroke:#333',
    'classDef default fill:#fff',
    'class A,B hot',
    'linkStyle default stroke:#00f',
    'A --> B; style B fill:#ff0',
    '%% 整行注释 end[x]',
  ]);
});

// 场景:子图标题带空格或符号却没写成 `名字["标题"]`。
// 期望:单个词的标题、已经带引号的标题不动;`名字[标题]` 只给标题补引号;其余的整句当标题,配一个
// 内部名字。
await test('flowchart subgraph titles with spaces or symbols get an id and quotes', () => {
  assert.deepEqual(repairedFlow(
    'subgraph 客户端 (Client)',
    'end',
    'subgraph 服务端',
    'end',
    'subgraph api[API 层 (v2)]',
    'end',
    'subgraph "带引号 的标题"',
    'end',
    'subgraph API 层 [v2]',
    'end',
  ), [
    'subgraph ace_sg_1["客户端 (Client)"]',
    'end',
    'subgraph 服务端',
    'end',
    'subgraph api["API 层 (v2)"]',
    'end',
    'subgraph "带引号 的标题"',
    'end',
    'subgraph ace_sg_2["API 层 [v2]"]',
    'end',
  ]);
});

// 场景:时序图的分组(loop / alt / opt / par / rect ...)没写 end,或者多写了一个 end;激活与取消
// 激活对不上(Mermaid 报 Trying to inactivate an inactive participant,整张图拒画)。
// 期望:缺的 end 补在末尾,多的 end 和落在分组外的 else 去掉;只有报了激活错误才去掉激活标记
// (消息都在,只是少了生命线上的激活条)。修好的文本官方解析器能过(时序图在 Node 里可以真解析)。
await test('sequence blocks are balanced and mismatched activations removed', async () => {
  const unclosed = 'sequenceDiagram\nloop 重试\nalt 成功\nA->>B: x\nelse 失败\nA->>B: y';
  const extra = 'sequenceDiagram\nA->>B: x\nend\nelse 多余\nA->>B: y';
  const activation = 'sequenceDiagram\nA->>+B: 请求\nactivate A\nB-->>-A: 响应\nB-->>-A: 再响应\ndeactivate A\nA-xB: 结束 +1 -1';
  const activationError = 'Trying to inactivate an inactive participant (B)';

  assert.equal(repairMermaidSource(unclosed, 'sequence'), `${unclosed}\nend\nend`);
  assert.equal(repairMermaidSource(extra, 'sequence'), 'sequenceDiagram\nA->>B: x\n\n\nA->>B: y');
  assert.equal(repairMermaidSource(activation, 'sequence'), activation);
  assert.equal(
    repairMermaidSource(activation, 'sequence', activationError),
    'sequenceDiagram\nA->>B: 请求\n\nB-->>A: 响应\nB-->>A: 再响应\n\nA-xB: 结束 +1 -1',
  );

  mermaid.initialize(mermaidConfig('light'));
  for (const [source, message] of [[unclosed, ''], [extra, ''], [activation, activationError]]) {
    assert.equal(await mermaid.parse(source, { suppressErrors: true }), false, source);
    assert.deepEqual(
      await mermaid.parse(repairMermaidSource(source, 'sequence', message), { suppressErrors: true }),
      { diagramType: 'sequence', config: {} },
      source,
    );
  }
});

// 场景:状态名带连字符(`pre-check`),官方解析器不认。
// 期望:声明 `state "pre-check" as pre_check`,连线和 state 语句里换成别名;冒号后面的说明文字、
// 开头的 `stateDiagram-v2`、箭头 `-->` 都不动。没有这种名字时原样返回。
await test('hyphenated state names become aliases', () => {
  assert.equal(
    repairMermaidSource('stateDiagram-v2\n[*] --> pre-check\npre-check --> post-check: a-b ok\nstate pre-check {\n  [*] --> x\n}', 'state'),
    [
      'stateDiagram-v2',
      'state "pre-check" as pre_check',
      'state "post-check" as post_check',
      '[*] --> pre_check',
      'pre_check --> post_check: a-b ok',
      'state pre_check {',
      '  [*] --> x',
      '}',
    ].join('\n'),
  );
  const fine = 'stateDiagram-v2\n[*] --> A\nA --> B: go-on';
  assert.equal(repairMermaidSource(fine, 'state'), fine);
});

// 场景:类名带 `::` 或连字符(C++ 的 `ns::Type`、`my-class`);成员签名里带花括号
// (`+g(opts: {a: number}) void`,会被当成成员区提前收尾)。
// 期望:成员区之外的这类名字包上反引号(Mermaid 认反引号里的任意名字),关系箭头和冒号后面的说明
// 不动;成员行里的花括号换成实体码,成员区收尾的那个 `}` 不动。
await test('class names with :: or hyphen get backticks, braces in members are escaped', () => {
  assert.equal(
    repairMermaidSource([
      'classDiagram',
      'class acecode::AgentLoop {',
      '  +g(opts: {a: number}) void',
      '  -std::string name_',
      '}',
      'class my-class',
      'my-class --> other-class : uses-it',
      'A <|-- B',
      'E o-- F',
      'namespace 领域 {',
      '  class std::string',
      '}',
    ].join('\n'), 'class'),
    [
      'classDiagram',
      'class `acecode::AgentLoop` {',
      '  +g(opts: #123;a: number#125;) void',
      '  -std::string name_',
      '}',
      'class `my-class`',
      '`my-class` --> `other-class` : uses-it',
      'A <|-- B',
      'E o-- F',
      'namespace 领域 {',
      '  class `std::string`',
      '}',
    ].join('\n'),
  );
});

// 场景:ER 图的关系后面漏了 `: 说明`(Mermaid 要求必须有)。
// 期望:补一个空说明;已经有说明的、不是关系的行不动。
await test('ER relationships without a label get an empty one', () => {
  assert.equal(
    repairMermaidSource('erDiagram\nA ||--o{ B\nC }o..o{ D : x\n"订单 表" ||--|{ ITEM\nUSER {\n  int id PK\n}', 'er'),
    'erDiagram\nA ||--o{ B : ""\nC }o..o{ D : x\n"订单 表" ||--|{ ITEM : ""\nUSER {\n  int id PK\n}',
  );
});

// 场景:修复只是补引号、改名、补 end,不应该凭空造出交互语句或标签。
// 期望:任意输入修完之后,没有新增 `<`,也没有新增以 click / style / properties 开头的行。
await test('repairs never introduce markup or new statements', () => {
  const inputs = [
    ['flowchart', flow('A[x <b> y] --> B[click me]', 'click[点] --> style[样]', 'subgraph click A "u"')],
    ['sequence', 'sequenceDiagram\nloop properties A: x\nA->>B: click'],
    ['state', 'stateDiagram-v2\na-b --> click-me: style x'],
    ['class', 'classDiagram\nclass a-b {\n  click {x}\n}'],
    ['er', 'erDiagram\nA ||--o{ B'],
  ];
  const statements = (text) => text.split('\n').filter((line) => /^\s*(?:click|style|classDef|linkStyle|links?|properties|details|callback)\s/i.test(line)).length;
  for (const [family, source] of inputs) {
    const repaired = repairMermaidSource(source, family);
    assert.equal((repaired.match(/</g) || []).length, (source.match(/</g) || []).length, source);
    assert.ok(statements(repaired) <= statements(source), source);
  }
  assert.equal(repairMermaidSource('pie\ntitle x', 'other'), 'pie\ntitle x');
});
