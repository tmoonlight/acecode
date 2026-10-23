import assert from 'node:assert/strict';
import {
  CONVERSATION_FIND_OPEN_EVENT,
  FIND_HISTORY_ITEM_LIMIT,
  applyFindHighlights,
  canOpenConversationFind,
  clearFindHighlights,
  collectFindMatches,
  conversationFindScope,
  findMatchesInText,
  findMatchesNeedRecollect,
  isComposingInputEvent,
  isFindMatchStale,
  isFindShortcut,
  requestConversationFindContent,
  scrollFindMatchIntoView,
  selectFindMatch,
} from './globalFind.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

async function runAsync(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// ---- 最小假 DOM:只实现查找与高亮用到的接口 ----
// Range 的偏移校验照浏览器行为:偏移超过节点长度抛 IndexSizeError。
function indexSizeError(offset, length) {
  const error = new Error(
    `IndexSizeError: The offset ${offset} is larger than the node's length (${length}).`,
  );
  error.name = 'IndexSizeError';
  return error;
}

class FakeNode {
  constructor(doc) {
    this.ownerDocument = doc;
    this.parentNode = null;
  }

  get isConnected() {
    let node = this;
    while (node.parentNode) node = node.parentNode;
    return node === this.ownerDocument;
  }

  get parentElement() {
    return this.parentNode?.nodeType === 1 ? this.parentNode : null;
  }
}

class FakeText extends FakeNode {
  constructor(doc, text) {
    super(doc);
    this.nodeType = 3;
    this.nodeValue = text;
  }

  get length() {
    return this.nodeValue.length;
  }

  splitText(offset) {
    const tail = new FakeText(this.ownerDocument, this.nodeValue.slice(offset));
    this.nodeValue = this.nodeValue.slice(0, offset);
    const parent = this.parentNode;
    if (parent) {
      const next = parent.childNodes[parent.childNodes.indexOf(this) + 1] || null;
      parent.insertBefore(tail, next);
    }
    return tail;
  }

  cloneNode() {
    return new FakeText(this.ownerDocument, this.nodeValue);
  }
}

class FakeElement extends FakeNode {
  constructor(doc, tagName) {
    super(doc);
    this.nodeType = 1;
    this.tagName = tagName.toUpperCase();
    this.childNodes = [];
    this.className = '';
    this.hidden = false;
    this.scrollCount = 0;
    const element = this;
    this.classList = {
      contains: (name) => element.className.split(/\s+/).includes(name),
      add: (name) => {
        if (!element.classList.contains(name)) {
          element.className = `${element.className} ${name}`.trim();
        }
      },
      toggle: (name, force) => {
        const names = element.className.split(/\s+/).filter((item) => item && item !== name);
        if (force) names.push(name);
        element.className = names.join(' ');
      },
    };
  }

  get firstChild() {
    return this.childNodes[0] || null;
  }

  getAttribute() {
    return null;
  }

  detach(node) {
    if (node.parentNode) node.parentNode.removeChild(node);
  }

  appendChild(node) {
    this.detach(node);
    node.parentNode = this;
    this.childNodes.push(node);
    return node;
  }

  insertBefore(node, reference) {
    this.detach(node);
    const index = reference ? this.childNodes.indexOf(reference) : -1;
    node.parentNode = this;
    if (index < 0) this.childNodes.push(node);
    else this.childNodes.splice(index, 0, node);
    return node;
  }

  removeChild(node) {
    const index = this.childNodes.indexOf(node);
    if (index >= 0) this.childNodes.splice(index, 1);
    node.parentNode = null;
    return node;
  }

  replaceChild(next, previous) {
    this.insertBefore(next, previous);
    this.removeChild(previous);
    return previous;
  }

  replaceChildren(...nodes) {
    for (const child of [...this.childNodes]) this.removeChild(child);
    for (const node of nodes) this.appendChild(node);
  }

  normalize() {
    const merged = [];
    for (const child of this.childNodes) {
      const last = merged[merged.length - 1];
      if (child.nodeType === 3 && last?.nodeType === 3) {
        last.nodeValue += child.nodeValue;
        child.parentNode = null;
      } else {
        merged.push(child);
      }
    }
    this.childNodes = merged;
  }

  querySelectorAll(selector) {
    const [, className] = /^mark\.(.+)$/.exec(selector) || [];
    const found = [];
    const visit = (node) => {
      for (const child of node.childNodes || []) {
        if (child.nodeType !== 1) continue;
        if (child.tagName === 'MARK' && child.classList.contains(className)) found.push(child);
        visit(child);
      }
    };
    visit(this);
    return found;
  }

  scrollIntoView() {
    this.scrollCount += 1;
  }
}

class FakeDocument {
  constructor() {
    this.nodeType = 9;
    this.parentNode = null;
    this.selectedRanges = [];
    const doc = this;
    this.defaultView = {
      innerHeight: 800,
      innerWidth: 1200,
      getComputedStyle: () => ({ display: 'block', visibility: 'visible' }),
      getSelection: () => ({
        removeAllRanges: () => { doc.selectedRanges = []; },
        addRange: (range) => { doc.selectedRanges.push(range); },
      }),
    };
    this.body = new FakeElement(this, 'body');
    this.body.parentNode = this;
  }

  createElement(tagName) {
    return new FakeElement(this, tagName);
  }

  createTextNode(text) {
    return new FakeText(this, text);
  }

  createRange() {
    const check = (node, offset) => {
      const length = node.nodeType === 3 ? node.length : node.childNodes.length;
      if (offset > length) throw indexSizeError(offset, length);
    };
    return {
      setStart(node, offset) { check(node, offset); this.start = { node, offset }; },
      setEnd(node, offset) { check(node, offset); this.end = { node, offset }; },
      selectNodeContents(node) { this.contents = node; },
      // 让匹配项落在视口外,走「滚动父元素」分支。
      getBoundingClientRect: () => ({ top: -500, bottom: -480, left: 0, right: 40 }),
    };
  }

  createTreeWalker(root, _whatToShow, filter) {
    const nodes = [];
    const visit = (node) => {
      if (node.nodeType === 3) {
        if (filter.acceptNode(node) === 1) nodes.push(node);
        return;
      }
      for (const child of node.childNodes || []) visit(child);
    };
    visit(root);
    let index = 0;
    return { nextNode: () => nodes[index++] || null };
  }
}

// 一条消息:57 个字符之后正好是查询词 abc,和白屏现场的偏移一致。
function messageFixture() {
  const doc = new FakeDocument();
  const root = doc.createElement('div');
  doc.body.appendChild(root);
  const paragraph = doc.createElement('p');
  root.appendChild(paragraph);
  const content = `${'x'.repeat(57)}abc tail`;
  const text = doc.createTextNode(content);
  paragraph.appendChild(text);
  return { doc, root, paragraph, text, content };
}

run('Ctrl+F opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'f', ctrlKey: true }), true);
});

run('Cmd+F opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F', metaKey: true }), true);
});

run('Alt+F is not treated as find shortcut', () => {
  assert.equal(isFindShortcut({ key: 'f', altKey: true }), false);
});

run('F3 opens app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F3' }), true);
});

run('Shift+F3 still routes to app find overlay', () => {
  assert.equal(isFindShortcut({ key: 'F3', shiftKey: true }), true);
});

run('Alt+F3 is left for the host', () => {
  assert.equal(isFindShortcut({ key: 'F3', altKey: true }), false);
});

run('conversation find is enabled only for an unblocked active single-session view', () => {
  const visible = {
    view: 'single',
    activeSessionId: 's1',
  };
  assert.equal(canOpenConversationFind(visible), true);
  assert.equal(canOpenConversationFind({ ...visible, activeSessionId: '' }), false);
  assert.equal(canOpenConversationFind({ ...visible, view: 'grid4' }), false);
  assert.equal(canOpenConversationFind({ ...visible, loop: true }), false);
  for (const blocker of [
    'showSettings',
    'searchOpen',
    'updateDialogOpen',
    'permissionOpen',
    'questionOpen',
    'guidedTourPreparing',
    'guidedTourRun',
  ]) {
    assert.equal(canOpenConversationFind({ ...visible, [blocker]: true }), false, blocker);
  }
});

run('findMatchesInText finds case-insensitive non-overlapping matches', () => {
  assert.deepEqual(findMatchesInText('Green green GREEN', 'green'), [
    { start: 0, end: 5 },
    { start: 6, end: 11 },
    { start: 12, end: 17 },
  ]);
});

run('findMatchesInText handles CJK text', () => {
  assert.deepEqual(findMatchesInText('绿色和深绿色', '绿色'), [
    { start: 0, end: 2 },
    { start: 4, end: 6 },
  ]);
});

run('findMatchesInText ignores empty query', () => {
  assert.deepEqual(findMatchesInText('abc', ''), []);
});

run('isComposingInputEvent detects native and ref composition state', () => {
  assert.equal(isComposingInputEvent({ isComposing: true }, false), true);
  assert.equal(isComposingInputEvent({ nativeEvent: { isComposing: true } }, false), true);
  assert.equal(isComposingInputEvent({}, true), true);
  assert.equal(isComposingInputEvent({}, false), false);
});

run('高亮切短节点后对话区重绘,跳转到旧匹配不再抛 IndexSizeError', () => {
  // 场景:长会话 Ctrl+F,匹配收集后高亮把文字节点切短到 57 个字符,随后历史
  // 分页加载让这条消息重绘,<mark> 连同旧节点被换掉;用户按回车跳到这一项。
  // 期望:旧匹配被判为失效;滚动 / 选中都不抛错,只是什么也不做。
  // 回归:修复前用旧偏移 setEnd(node, 60) 抛 IndexSizeError,错误出在 React
  // 副作用里又没有错误边界,整个页面被卸载成空白(2026-10-23 现场)。
  const { doc, root, paragraph, text, content } = messageFixture();
  const matches = collectFindMatches(root, 'abc');
  assert.equal(matches.length, 1);
  assert.equal(findMatchesNeedRecollect(matches), false);

  assert.equal(applyFindHighlights(matches, 0, doc), true);
  assert.equal(text.nodeValue.length, 57);
  assert.equal(matches[0].mark.isConnected, true);
  assert.equal(findMatchesNeedRecollect(matches), false);

  paragraph.replaceChildren(doc.createTextNode(content));
  assert.equal(matches[0].mark.isConnected, false);
  // 假 DOM 与浏览器一致:直接用旧偏移建 Range 必然抛错。
  assert.throws(() => {
    const range = doc.createRange();
    range.setStart(text, matches[0].start);
    range.setEnd(text, matches[0].end);
  }, /IndexSizeError/);

  assert.equal(isFindMatchStale(matches[0]), true);
  assert.equal(findMatchesNeedRecollect(matches), true);
  assert.doesNotThrow(() => scrollFindMatchIntoView(matches[0], doc));
  assert.doesNotThrow(() => selectFindMatch(matches[0], doc));
  assert.equal(doc.selectedRanges.length, 0);
});

run('重绘后重新收集得到新节点上的匹配,可以正常高亮与滚动', () => {
  // 场景:上一条的失效匹配触发重新收集。
  // 期望:新匹配指向重绘后的文字节点;高亮后滚动的是新的 <mark>。
  const { doc, root, paragraph, content } = messageFixture();
  const stale = collectFindMatches(root, 'abc');
  applyFindHighlights(stale, 0, doc);
  paragraph.replaceChildren(doc.createTextNode(content));

  clearFindHighlights(doc);
  const fresh = collectFindMatches(root, 'abc');
  assert.equal(fresh.length, 1);
  assert.equal(fresh[0].node, paragraph.firstChild);
  assert.equal(findMatchesNeedRecollect(fresh), false);

  assert.equal(applyFindHighlights(fresh, 0, doc), true);
  scrollFindMatchIntoView(fresh[0], doc);
  assert.equal(fresh[0].mark.scrollCount, 1);
});

run('未高亮的匹配:节点脱离文档、长度不够或文字被改写都算失效', () => {
  // 场景:匹配收集后还没来得及高亮,React 就更新了这段文字。
  // 期望:三种情况都判失效;文字没变时仍然可用,并且能按偏移选中。
  const fixture = () => {
    const { doc, root, text } = messageFixture();
    return { doc, text, match: collectFindMatches(root, 'ABC')[0] };
  };

  const intact = fixture();
  assert.equal(isFindMatchStale(intact.match), false);
  selectFindMatch(intact.match, intact.doc);
  assert.equal(intact.doc.selectedRanges.length, 1);

  const detached = fixture();
  detached.text.parentNode.removeChild(detached.text);
  assert.equal(isFindMatchStale(detached.match), true);

  const shortened = fixture();
  shortened.text.nodeValue = 'x'.repeat(58);
  assert.equal(isFindMatchStale(shortened.match), true);

  // 长度不变但这几个字已经不是查询词(流式输出改写了同一节点)。
  const rewritten = fixture();
  rewritten.text.nodeValue = `${'x'.repeat(57)}xyz tail`;
  assert.equal(isFindMatchStale(rewritten.match), true);
  selectFindMatch(rewritten.match, rewritten.doc);
  assert.equal(rewritten.doc.selectedRanges.length, 0);
});

await runAsync('打开查找时等 ChatView 交回的全部加载任务完成', async () => {
  // 场景:Ctrl+F 广播打开事件,ChatView 开始逐页加载全部历史并经 waitUntil 交回任务。
  // 期望:loading 为 true;ready 要等任务完成才兑现,浮层在那之后才收集匹配。
  const target = new EventTarget();
  let finish;
  const task = new Promise((resolve) => { finish = resolve; });
  target.addEventListener(CONVERSATION_FIND_OPEN_EVENT, (event) => {
    event.detail.waitUntil(task);
  });
  const request = requestConversationFindContent(target);
  assert.equal(request.loading, true);

  let ready = false;
  const settled = request.ready.then(() => { ready = true; });
  await Promise.resolve();
  assert.equal(ready, false);
  finish();
  await settled;
  assert.equal(ready, true);
});

run('查找范围:还有更早历史时为部分搜索,只数对话消息', () => {
  // 场景:查找带上限加载后,transcript 里既有消息也有工具行,服务端仍报还有更早的历史。
  // 期望:partial 为 true,messageCount 只数 kind 为 msg 的条目(工具行不算消息);
  // 历史已经到头时 partial 为 false,浮层不显示提示。
  const items = [
    { kind: 'msg' }, { kind: 'tool' }, { kind: 'msg' }, { kind: 'termination_notice' }, { kind: 'msg' },
  ];
  assert.deepEqual(conversationFindScope({ items, historyHasMore: true }), { partial: true, messageCount: 3 });
  assert.deepEqual(conversationFindScope({ items, historyHasMore: false }), { partial: false, messageCount: 3 });
  assert.deepEqual(conversationFindScope(null), { partial: false, messageCount: 0 });
  assert.ok(FIND_HISTORY_ITEM_LIMIT > 0);
});

await runAsync('ready 带回 ChatView 交回的搜索范围', async () => {
  // 场景:ChatView 的加载任务兑现为部分搜索范围(只加载了最近 4321 条消息)。
  // 期望:浮层拿到的 ready 兑现值就是这个范围,用来显示「只搜索了最近 4321 条消息」;
  // 没有部分范围时兑现为 partial=false。
  const target = new EventTarget();
  target.addEventListener(CONVERSATION_FIND_OPEN_EVENT, (event) => {
    event.detail.waitUntil(Promise.resolve({ partial: true, messageCount: 4321 }));
  });
  assert.deepEqual(await requestConversationFindContent(target).ready, { partial: true, messageCount: 4321 });

  const complete = new EventTarget();
  complete.addEventListener(CONVERSATION_FIND_OPEN_EVENT, (event) => {
    event.detail.waitUntil(Promise.resolve({ partial: false, messageCount: 80 }));
  });
  assert.deepEqual(await requestConversationFindContent(complete).ready, { partial: false, messageCount: 0 });
});

await runAsync('没有监听方或加载失败时不卡住查找', async () => {
  // 场景:当前没有 ChatView(或它不交任务),以及历史加载报错。
  // 期望:前者 loading 为 false 立即可收集;后者 ready 照样兑现,不让浮层一直停在「加载中」。
  const idle = requestConversationFindContent(new EventTarget());
  assert.equal(idle.loading, false);
  await idle.ready;

  const target = new EventTarget();
  target.addEventListener(CONVERSATION_FIND_OPEN_EVENT, (event) => {
    event.detail.waitUntil(Promise.reject(new Error('history page failed')));
    event.detail.waitUntil('not a task');
  });
  const failed = requestConversationFindContent(target);
  assert.equal(failed.loading, true);
  await failed.ready;
});
