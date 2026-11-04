import assert from 'node:assert/strict';
import vm from 'node:vm';
import { DOMParser } from '@xmldom/xmldom';
import { inlineVisualRuntime } from './inlineVisualRuntime.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const XHTML = 'http://www.w3.org/1999/xhtml';
const SVG = 'http://www.w3.org/2000/svg';

// 假 DOM:xmldom 文档里放 #ace-root 与 #ace-theme;解析函数把(格式良好的)片段包一层后解析,
// 代替浏览器的 <template>.innerHTML。只用于验证对齐、剥离与消息逻辑,不执行脚本。
function harness({ activation = null, now = 0 } = {}) {
  const parser = new DOMParser();
  const doc = parser.parseFromString(
    `<html xmlns="${XHTML}" data-mode="light"><head><style id="ace-theme"></style></head>`
    + '<body><div id="ace-root"></div></body></html>',
    'text/xml',
  );
  const root = doc.getElementById('ace-root');
  const posted = [];
  const clock = { now };
  const win = {
    navigator: activation ? { userActivation: activation } : {},
    setTimeout: () => 0,
    clearTimeout: () => {},
  };
  const runtime = inlineVisualRuntime(
    { protocol: 1, mode: 'light', palette: ['#111111', 'bad'], sendMaxChars: 4000, labels: {} },
    {
      window: win,
      document: doc,
      root,
      boot: false,
      now: () => clock.now,
      post: (message) => posted.push(message),
      parse: (code) => parser.parseFromString(`<x xmlns="${XHTML}">${code}</x>`, 'text/xml').documentElement,
    },
  );
  return { runtime, doc, root, posted, clock, win };
}

function elementChildren(node) {
  return Array.from({ length: node.childNodes.length }, (_, index) => node.childNodes[index])
    .filter((child) => child.nodeType === 1);
}

// 触发场景:流式输出到一半,visual 片段里已经有完整的 <script>…</script>,末尾还有一个
// 只写了开头的 <script。
// 期望行为:两段都被剥掉(大小写不敏感),脚本前面的内容原样保留 —— 流式期间不执行任何脚本。
run('流式期间剥掉完整与写了一半的 script', () => {
  const { runtime } = harness();
  assert.equal(
    runtime.stripScripts('<div>a</div><SCRIPT type="x">alert(1)</script ><p>b</p><script>let half = 1'),
    '<div>a</div><p>b</p>',
  );
  assert.equal(runtime.stripScripts('<svg></svg><scr'), '<svg></svg><scr');
  assert.equal(runtime.stripScripts('<svg></svg><script'), '<svg></svg>');
  assert.equal(runtime.stripScripts(''), '');
});

// 触发场景:一段 SVG 分多次追加输出(先写了一个矩形,后面又写出第二个并改了第一个的属性)。
// 期望行为:隔离框里第一个矩形还是原来那个节点(属性原地更新,不闪烁重建),第二个追加在后面;
// 内容变短时多出的节点被删掉。
run('流式差量对齐保留已画出的节点', () => {
  const { runtime, root } = harness();
  runtime.render(`<svg xmlns="${SVG}" viewBox="0 0 10 10"><rect class="node" x="1"/></svg>`, false);
  const svg = elementChildren(root)[0];
  const first = elementChildren(svg)[0];
  assert.equal(svg.namespaceURI, SVG);
  assert.equal(first.getAttribute('x'), '1');

  runtime.render(`<svg xmlns="${SVG}" viewBox="0 0 10 10"><rect class="node" x="2"/><circle r="3"/></svg>`, false);
  assert.equal(elementChildren(root)[0], svg);
  assert.equal(elementChildren(svg)[0], first);
  assert.equal(first.getAttribute('x'), '2');
  assert.equal(elementChildren(svg)[1].localName, 'circle');

  runtime.render(`<svg xmlns="${SVG}" viewBox="0 0 10 10"><rect class="node" x="2"/></svg>`, false);
  assert.equal(elementChildren(svg).length, 1);
  assert.equal(elementChildren(svg)[0], first);
});

// 触发场景:模型写了 onclick / onload 属性。流式期间这些属性不能生效(onload 一插入就会执行),
// 定稿后才挂上;定稿时脚本按文档顺序换成新建的 script(浏览器里由此执行)。
// 期望行为:流式时节点上没有 on* 属性;定稿后有;script 被替换成新节点且正文不变;
// 定稿之后再来的内容被忽略(外层会换新 iframe)。
run('on* 属性与脚本只在定稿后生效', () => {
  const { runtime, root } = harness();
  runtime.render('<button onclick="ace.send(\'q\')" class="primary">问</button><script>window.ran = 1</script>', false);
  const button = elementChildren(root)[0];
  assert.equal(button.getAttribute('class'), 'primary');
  assert.equal(button.hasAttribute('onclick'), false);
  assert.equal(elementChildren(root).length, 1);

  runtime.render('<button onclick="ace.send(\'q\')" class="primary">问</button><script>window.ran = 1</script>', true);
  assert.equal(elementChildren(root)[0], button);
  assert.equal(button.getAttribute('onclick'), "ace.send('q')");
  const script = elementChildren(root)[1];
  assert.equal(script.localName, 'script');
  assert.equal(script.text, 'window.ran = 1');
  assert.equal(runtime.state.finalized, true);

  runtime.render('<p>late</p>', true);
  assert.equal(elementChildren(root)[0], button);
});

// 触发场景:模型(或被注入的内容)写了 meta refresh、base、iframe、object、embed 或预取链接,
// 试图让隔离框导航走或嵌入别的文档。
// 期望行为:这些元素被丢掉;样式表 link 保留(来源由内容安全策略限制)。
run('丢掉能让框离开自身文档的元素', () => {
  const { runtime, root } = harness();
  runtime.render(
    '<meta http-equiv="refresh" content="0;url=https://evil.example"/><base href="https://evil.example/"/>'
    + '<iframe src="https://evil.example"></iframe><object data="x"></object><embed src="x"/>'
    + '<link rel="prefetch" href="https://evil.example"/><link rel="stylesheet" href="https://cdn.jsdelivr.net/x.css"/>'
    + '<div class="card"><iframe></iframe>ok</div>',
    false,
  );
  const names = elementChildren(root).map((node) => node.localName);
  assert.deepEqual(names, ['link', 'div']);
  assert.equal(elementChildren(root)[0].getAttribute('rel'), 'stylesheet');
  assert.equal(elementChildren(elementChildren(root)[1]).length, 0);
});

// 触发场景:框内脚本在没有用户操作时调用 ace.send(循环刷消息),以及用户真实点击后调用。
// 期望行为:没有用户激活时一条都不发;激活后发出去掉首尾空白的文本;空 / 非字符串 / 超长忽略;
// 没有 userActivation 接口时看 1 秒内有没有可信的用户事件。
run('追问只在用户刚操作过时发出', () => {
  const activation = { isActive: false };
  const { runtime, posted } = harness({ activation });
  assert.equal(runtime.api.send('自动刷消息'), false);
  activation.isActive = true;
  assert.equal(runtime.api.send('  讲讲缓存层 '), true);
  assert.equal(runtime.api.send(''), false);
  assert.equal(runtime.api.send(42), false);
  assert.equal(runtime.api.send('x'.repeat(4001)), false);
  assert.deepEqual(posted, [{ type: 'send', text: '讲讲缓存层', ace_visual: 1 }]);

  const fallback = harness({ now: 5000 });
  assert.equal(fallback.runtime.api.send('q'), false);
  fallback.runtime.state.lastTrusted = 4500;
  assert.equal(fallback.runtime.api.send('q'), true);
  fallback.clock.now = 6000;
  assert.equal(fallback.runtime.api.send('q'), false);
});

// 触发场景:框内链接点击、ace.openLink、window.open。
// 期望行为:只有 http / https / mailto 且用户刚操作过才交给外层;javascript: 等一律无效。
run('框内链接只放行 http / https / mailto', () => {
  const activation = { isActive: true };
  const { runtime, posted } = harness({ activation });
  assert.equal(runtime.api.openLink('https://example.com'), true);
  assert.equal(runtime.api.openLink('javascript:alert(1)'), false);
  assert.equal(runtime.api.openLink('/api/sessions'), false);
  activation.isActive = false;
  assert.equal(runtime.api.openLink('https://example.com/2'), false);
  assert.deepEqual(posted, [{ type: 'link', url: 'https://example.com', ace_visual: 1 }]);
});

// 触发场景:用户在 visual 显示后把外观切到深色。
// 期望行为:只换 data-mode、主题变量与调色板(内容不动);可疑的变量值被丢掉;
// onThemeChange 注册的回调收到新模式与调色板(canvas 图表据此重设颜色)。
run('主题切换原地更新并通知图表', () => {
  const { runtime, doc } = harness();
  const seen = [];
  runtime.api.onThemeChange((value) => seen.push(value));
  assert.deepEqual(runtime.api.palette, ['#111111']);
  runtime.handleMessage({
    ace_visual: 1, type: 'theme', mode: 'dark',
    vars: { '--fg': '#bfbfbf', '--accent': 'x}html{display:none', bad: 'red' },
    palette: ['#60a5fa', 'nope', '#2dd4bf'],
  });
  assert.equal(doc.documentElement.getAttribute('data-mode'), 'dark');
  assert.equal(runtime.api.mode, 'dark');
  assert.deepEqual(runtime.api.palette, ['#60a5fa', '#2dd4bf']);
  assert.deepEqual(seen, [{ mode: 'dark', palette: ['#60a5fa', '#2dd4bf'] }]);
  assert.equal(runtime.themeCss({ '--fg': '#bfbfbf', '--accent': 'x}html{', bad: 'red' }), ':root{--fg:#bfbfbf;}');
});

// 触发场景:外层发来没有协议标记的消息(别的脚本也在 postMessage)。
// 期望行为:忽略;带标记的 render 正常处理。
run('只处理带协议标记的消息', () => {
  const { runtime, root } = harness();
  runtime.handleMessage({ type: 'render', code: '<p>x</p>', final: false });
  assert.equal(root.childNodes.length, 0);
  runtime.handleMessage({ ace_visual: 1, type: 'render', code: '<p>x</p>', final: false });
  assert.equal(elementChildren(root)[0].localName, 'p');
});

// 触发场景:不透明源里访问 localStorage 会抛错,模型写的页面常常无条件读写它。
// 期望行为:内存版存储按 Storage 接口工作(刷新即丢)。
run('内存版存储', () => {
  const { runtime } = harness();
  const storage = runtime.memoryStorage();
  storage.setItem('a', 1);
  assert.equal(storage.getItem('a'), '1');
  assert.equal(storage.length, 1);
  assert.equal(storage.key(0), 'a');
  storage.removeItem('a');
  assert.equal(storage.getItem('a'), null);
  storage.setItem('b', 'x');
  storage.clear();
  assert.equal(storage.length, 0);
});

// 触发场景:运行时被 Function.prototype.toString 拼进隔离框,在一个没有本模块任何东西的
// 全新环境里启动。回归:若函数体引用了 import 或模块顶层声明,框里会直接 ReferenceError。
// 期望行为:在只有基础全局对象的新上下文里也能求值并完成剥离与渲染。
run('运行时函数可以脱离本模块独立运行', () => {
  const isolated = vm.runInNewContext(`(${inlineVisualRuntime.toString()})`, {});
  const { doc, root } = harness();
  const parser = new DOMParser();
  const instance = isolated({ protocol: 1, labels: {} }, {
    window: {},
    document: doc,
    root,
    boot: false,
    post: () => {},
    parse: (code) => parser.parseFromString(`<x xmlns="${XHTML}">${code}</x>`, 'text/xml').documentElement,
  });
  assert.equal(instance.stripScripts('<p>a</p><script>1</script>'), '<p>a</p>');
  instance.render('<p>a</p>', false);
  assert.equal(elementChildren(root)[0].localName, 'p');
});
