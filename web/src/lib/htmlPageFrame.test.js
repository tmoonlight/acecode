// HTML 文件预览隔离框的外层纯逻辑与安全结构守卫(htmlPageFrame.js / htmlPageRuntime.js)。
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { parse } from 'parse5';
import {
  HTML_PAGE_PROTOCOL,
  HTML_PAGE_SANDBOX,
  buildHtmlPageSrcdoc,
  cssColorToHex,
  cssPixels,
  findDoctypeEnd,
  htmlElementCapabilities,
  htmlPageMessage,
  htmlSiteBaseHref,
  htmlSiteLinkToPath,
  htmlSiteRootUrl,
  isBoldWeight,
  isWindowsRedoKey,
  normalizeTextCaret,
  normalizeTextRange,
  splitRangeStyles,
  toggledDecoration,
  toolbarMenuPlacement,
} from './htmlPageFrame.js';
import { htmlPageRuntime } from './htmlPageRuntime.js';
import { indexHtmlSource } from './htmlSourceMap.js';

function source(path) {
  return fs.readFileSync(new URL(path, import.meta.url), 'utf8');
}

function withoutComments(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:'"\\])\/\/.*$/gm, '$1');
}

// 场景:浏览一份带 doctype 的报告页。期望:<base> 与运行时插在 doctype 之后(页面不会掉进怪异模式),
// 其余内容逐字节不变;页面自己写了 <base href> 时不再插我们的。
{
  const page = '<!-- c -->\n<!DOCTYPE html>\n<html><body><img src="a.png"></body></html>';
  const at = findDoctypeEnd(page);
  assert.equal(page.slice(0, at), '<!-- c -->\n<!DOCTYPE html>');
  const srcdoc = buildHtmlPageSrcdoc({ text: page, baseHref: 'http://127.0.0.1:1/api/html-site/x/d/', config: { nonce: 'n' } });
  assert.ok(srcdoc.startsWith('<!-- c -->\n<!DOCTYPE html><base href="http://127.0.0.1:1/api/html-site/x/d/"><script data-ace-html-runtime>'));
  assert.ok(srcdoc.endsWith('</script>\n<html><body><img src="a.png"></body></html>'));
  const own = '<html><head><base href="https://cdn.example/"></head></html>';
  assert.equal(buildHtmlPageSrcdoc({ text: own, baseHref: 'http://x/' }).includes('<base href="http://x/">'), false);
  assert.equal(findDoctypeEnd('<html></html>'), -1);
  console.log('[pass] HTML page srcdoc injects base and runtime after the doctype');
}

// 场景:编辑模式渲染。期望:副本里有元素编号,运行时配置里带 nonce 与行内标签表;
// 页面里出现 </script> 字样的配置值不会提前结束注入的脚本。
{
  const page = '<!DOCTYPE html><body><h1>T</h1></body>';
  const index = indexHtmlSource(page, parse);
  const srcdoc = buildHtmlPageSrcdoc({
    text: page, index, doctypeEnd: index.doctypeEnd, config: { nonce: 'abc', siteBase: '</script><x>' },
  });
  assert.ok(srcdoc.includes('<h1 data-ace-sid="1">T</h1>'));
  assert.ok(srcdoc.includes('"nonce":"abc"'));
  assert.ok(srcdoc.includes('"inlineTags":['));
  assert.equal(srcdoc.split('</script>').length, 2);
  console.log('[pass] HTML page srcdoc instruments elements in edit mode');
}

// 场景:别的 iframe 或上一次渲染留下的旧文档发来消息。期望:协议号、nonce、类型都对才处理。
// (nonce 不是鉴权:框内页面脚本读得到它,有后果的消息在 HtmlPagePreview 里另要求用户激活。)
{
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'n', type: 'select' }, 'n').type, 'select');
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'm', type: 'select' }, 'n'), null);
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'n', type: 'eval' }, 'n'), null);
  assert.equal(htmlPageMessage({ ace_html: 2, nonce: 'n', type: 'select' }, 'n'), null);
  assert.equal(htmlPageMessage(null, 'n'), null);
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, type: 'select' }, ''), null);
  console.log('[pass] HTML page messages require protocol, nonce and known type');
}

// 场景:页面里点了指向同目录另一个 HTML 的链接。期望:换回本地绝对路径(Windows 用反斜杠、
// 中文与空格解码);带 ..、空段、编码坏掉的一律不处理。
{
  const entryUrl = 'http://127.0.0.1:9/api/html-site/abc/docs/%E6%8A%A5%E5%91%8A.html';
  assert.equal(htmlSiteRootUrl(entryUrl, 'docs/%E6%8A%A5%E5%91%8A.html'), 'http://127.0.0.1:9/api/html-site/abc/');
  assert.equal(htmlSiteBaseHref(entryUrl), 'http://127.0.0.1:9/api/html-site/abc/docs/');
  assert.equal(htmlSiteLinkToPath({
    absoluteFile: 'N:\\work\\docs\\报告.html', entry: 'docs/%E6%8A%A5%E5%91%8A.html', rest: 'docs/a%20b.html',
  }), 'N:\\work\\docs\\a b.html');
  assert.equal(htmlSiteLinkToPath({
    absoluteFile: '/home/me/site/index.html', entry: 'site/index.html', rest: 'site/next.html',
  }), '/home/me/site/next.html');
  // 回归:文件详情给的是正斜杠形式的 Windows 路径(N:/…)时,曾因按反斜杠比对入口而打不开站内链接。
  assert.equal(htmlSiteLinkToPath({
    absoluteFile: 'N:/Work/report/index.html', entry: 'report/index.html', rest: 'report/page2.html',
  }), 'N:/Work/report/page2.html');
  // 盘符路径不区分大小写:入口大小写与磁盘不同也能对上。
  assert.equal(htmlSiteLinkToPath({
    absoluteFile: 'n:\\work\\REPORT\\index.html', entry: 'report/index.html', rest: 'report/a.html',
  }), 'n:\\work\\report\\a.html');
  assert.equal(htmlSiteLinkToPath({ absoluteFile: '/a/index.html', entry: 'index.html', rest: '../x.html' }), '');
  assert.equal(htmlSiteLinkToPath({ absoluteFile: '/a/index.html', entry: 'index.html', rest: '%E0%A4' }), '');
  assert.equal(htmlSiteLinkToPath({ absoluteFile: '/a/index.html', entry: 'other.html', rest: 'x.html' }), '');
  console.log('[pass] HTML site links map back to local files');
}

// 场景:工具栏读选中元素的计算样式。期望:颜色转成颜色选择器认的 #rrggbb(透明给空),
// 字号取像素数,600 以上算加粗,下划线 / 删除线按项增减。
{
  assert.equal(cssColorToHex('rgb(255, 0, 16)'), '#ff0010');
  assert.equal(cssColorToHex('rgba(0, 0, 0, 0)'), '');
  assert.equal(cssColorToHex('rgb(12 34 56 / 50%)'), '#0c2238');
  assert.equal(cssColorToHex('#ABC'), '#aabbcc');
  assert.equal(cssColorToHex('transparent'), '');
  assert.equal(cssPixels('30px'), 30);
  assert.equal(cssPixels('13.333px'), 13.3);
  assert.equal(isBoldWeight('700'), true);
  assert.equal(isBoldWeight('400'), false);
  assert.equal(toggledDecoration('none', 'underline'), 'underline');
  assert.equal(toggledDecoration('underline line-through', 'underline'), 'line-through');
  assert.equal(toggledDecoration('line-through', 'line-through'), 'none');
  console.log('[pass] HTML toolbar values derive from computed styles');
}

// 场景:选中脚本生成的元素、被脚本克隆出重复编号的元素、普通标题、省略结束标签的段落。
// 期望:前两种只能交给 AI 编辑;标题全部能力都有;段落能改样式但不能直接改字或包链接。
{
  const h1 = { tag: 'h1', textLeaf: true, hasEndTag: true };
  const p = { tag: 'p', textLeaf: false, hasEndTag: false };
  assert.equal(htmlElementCapabilities({ sid: 0, tag: 'tr', textLeaf: false }, null).mapped, false);
  assert.equal(htmlElementCapabilities({ sid: 3, duplicate: true, tag: 'h1', textLeaf: true }, h1).mapped, false);
  assert.deepEqual(htmlElementCapabilities({ sid: 3, tag: 'h1', textLeaf: true }, h1),
    { mapped: true, style: true, text: true, link: true, image: false, remove: true });
  const para = htmlElementCapabilities({ sid: 4, tag: 'p', textLeaf: true }, p);
  assert.equal(para.style && !para.text && !para.link, true);
  console.log('[pass] HTML element capabilities follow the source mapping');
}

// 触发场景:有人给预览隔离框加 allow-same-origin(本机回环免 token,页面脚本就能调全部 daemon
// 接口)或 allow-popups / allow-top-navigation / allow-forms。
// 期望行为:sandbox 恰好是 allow-scripts + allow-modals;组件只用这个常量。
{
  assert.equal(HTML_PAGE_SANDBOX, 'allow-scripts allow-modals');
  const component = source('../components/HtmlPagePreview.jsx');
  assert.ok(component.includes('sandbox={HTML_PAGE_SANDBOX}'));
  assert.equal(/allow-same-origin|allow-top-navigation|allow-popups|allow-forms/.test(component), false);
  console.log('[pass] HTML preview sandbox stays opaque');
}

// 触发场景:框内运行时被 toString 拼进隔离框,构建期的文案本地化插件会把中文字符串字面量换成
// 框里不存在的函数调用,整个运行时启动即报错。
// 期望行为:运行时与外壳模块(去掉注释后)不含中文;运行时不 import;序列化后是合法函数。
{
  const han = new RegExp('[\\u3400-\\u9fff]');
  assert.equal(han.test(withoutComments(source('./htmlPageRuntime.js'))), false);
  assert.equal(han.test(withoutComments(source('./htmlPageFrame.js'))), false);
  assert.equal(/^\s*import\s/m.test(source('./htmlPageRuntime.js')), false);
  assert.equal(typeof new Function(`return (${htmlPageRuntime.toString()});`)(), 'function');
  console.log('[pass] HTML page runtime is self-contained');
}

// 场景:用户在 Windows 上按 Ctrl+Y 重做(程序里的输入框与富文本编辑器都这么认)。期望:Ctrl+Y 算重做;
// Ctrl+Shift+Y、Alt / Meta 组合、macOS 上的 Ctrl+Y 不算(macOS 的重做是 Cmd+Shift+Z,由配置的快捷键判定)。
{
  assert.equal(isWindowsRedoKey({ key: 'y', code: 'KeyY', ctrlKey: true }), true);
  assert.equal(isWindowsRedoKey({ key: 'Y', code: 'KeyY', ctrlKey: true }), true);
  assert.equal(isWindowsRedoKey({ key: 'y', code: 'KeyY', ctrlKey: true, shiftKey: true }), false);
  assert.equal(isWindowsRedoKey({ key: 'y', code: 'KeyY', ctrlKey: true, altKey: true }), false);
  assert.equal(isWindowsRedoKey({ key: 'y', code: 'KeyY', metaKey: true }), false);
  assert.equal(isWindowsRedoKey({ key: 'y', code: 'KeyY', ctrlKey: true }, true), false);
  console.log('[pass] HTML editor accepts Ctrl+Y as redo on Windows');
}

// 触发场景:用户改完文字(失去焦点提交)后撤销,页面整体闪一下 —— 撤销曾经是把整页重新加载。
// 期望行为:撤销 / 重做只在框内原地重放反向 / 正向操作,不重新生成 srcdoc,也不走外部改动的重渲染。
{
  const component = source('../components/HtmlPagePreview.jsx');
  const replay = component.slice(component.indexOf('const replay = useCallback'), component.indexOf('const undo = useCallback'));
  assert.ok(replay.includes('post(op)'));
  assert.equal(/renderFrame|setFrame|keepHistory/.test(replay), false);
  const runtime = source('./htmlPageRuntime.js');
  for (const op of ["case 'setInner'", "case 'restore'", "case 'unwrapLink'"]) assert.ok(runtime.includes(op), op);
  console.log('[pass] HTML undo and redo replay in place without reloading the frame');
}

// 场景:工具栏的下拉(字号、对齐、颜色)在预览区域里展开。期望:默认贴在按钮正下方、左对齐;
// 靠近右边放不下时改成右对齐;靠近底部、上方更宽敞时向上展开;高度限制在可用空间内,最多约 8 行。
{
  const bounds = { left: 0, top: 0, right: 600, bottom: 500 };
  const near = toolbarMenuPlacement({ trigger: { left: 100, right: 130, top: 40, bottom: 70 }, menu: { width: 160, height: 300 }, bounds });
  assert.deepEqual(near, { alignRight: false, above: false, maxHeight: 320 });
  const right = toolbarMenuPlacement({ trigger: { left: 520, right: 550, top: 40, bottom: 70 }, menu: { width: 160, height: 200 }, bounds });
  assert.equal(right.alignRight, true);
  const low = toolbarMenuPlacement({ trigger: { left: 100, right: 130, top: 430, bottom: 460 }, menu: { width: 160, height: 300 }, bounds });
  assert.equal(low.above, true);
  assert.equal(low.maxHeight, 320);
  // 可用空间比上限小时按可用空间算。
  assert.equal(toolbarMenuPlacement({ trigger: { left: 0, right: 30, top: 40, bottom: 70 }, menu: { width: 100, height: 300 }, bounds: { left: 0, top: 0, right: 600, bottom: 260 } }).maxHeight, 178);
  // 下方放得下就不向上翻,哪怕上方更宽敞。
  assert.equal(toolbarMenuPlacement({ trigger: { left: 0, right: 30, top: 300, bottom: 330 }, menu: { width: 100, height: 120 }, bounds }).above, false);
  console.log('[pass] HTML toolbar menus open right under their button and stay inside the preview');
}

// 触发场景:双击进入改字后,在框里拖选一段文字再点加粗 / 改颜色 / 改字号。
// 期望行为:文字格式(粗细、斜体、下划线 / 删除线、颜色、字号)交给框内只套在选中的文字上,
// 对齐是段落属性,仍改整个元素;框里报来的选区格式只留已知字段、转成短字符串。
{
  assert.deepEqual(splitRangeStyles({ 'font-weight': '700', 'text-align': 'center', color: '#dc2626' }), {
    range: { 'font-weight': '700', color: '#dc2626' },
    element: { 'text-align': 'center' },
  });
  assert.deepEqual(splitRangeStyles({ 'font-size': '24px' }), { range: { 'font-size': '24px' }, element: {} });
  assert.deepEqual(normalizeTextRange({
    text: 'brave', href: 'https://e.example/', extra: 1,
    styles: { fontSize: '24px', fontWeight: '700', color: { evil: 1 }, onclick: 'x' },
  }), { text: 'brave', href: 'https://e.example/', styles: { fontSize: '24px', fontWeight: '700' } });
  assert.equal(normalizeTextRange(null), null);
  assert.equal(normalizeTextRange({ text: 'x' }), null);
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'n', type: 'textSelection' }, 'n').type, 'textSelection');
  console.log('[pass] HTML text-edit selection formats only the selected text');
}

// 触发场景:双击进入改字后选中框多了一圈光晕、浏览器又画了一圈焦点框,工具栏也消失了。
// 期望行为:改字时选中框样式不变(没有 .select.text 光晕,可编辑元素的焦点框被去掉);工具栏在改字时照常渲染;
// 格式命令只在执行期间临时切到富文本编辑,结束切回纯文本(打字、粘贴、回车不会冒出 <div>)。
{
  const runtime = source('./htmlPageRuntime.js');
  assert.equal(runtime.includes('.select.text'), false);
  assert.ok(runtime.includes(':focus-visible{outline:none!important;}'));
  assert.ok(runtime.includes("case 'formatRange'"));
  const rich = runtime.slice(runtime.indexOf('function withRichEditing'), runtime.indexOf('function richCommand'));
  assert.ok(rich.includes('finally'));
  assert.ok(rich.includes("host.setAttribute('contenteditable', previous)"));
  // 拖选拖过头在框外松开:那次 click 派给共同祖先,不能当成点了别处而结束改字。
  const click = runtime.slice(runtime.indexOf("window.addEventListener('click', function (event) {\n    var fromText"));
  assert.ok(click.indexOf('fromText && textEditing') >= 0 && click.indexOf('fromText && textEditing') < click.indexOf('select(pick(event.target))'));
  const component = source('../components/HtmlPagePreview.jsx');
  assert.equal(/!textEditing\s*&&\s*\(\s*<HtmlEditToolbar/.test(component), false);
  assert.ok(/caps && \(\s*<HtmlEditToolbar/.test(component));
  console.log('[pass] HTML text editing keeps the selection box and the toolbar');
}

// 触发场景:编辑模式里在页面上按下拖动画选区(WorkBuddy 的框选)。
// 期望行为:按下后移动超过阈值才开始画,松开后的那次 click 被吞掉、不当成选元素;画选区时页面自己的文字选择
// 与拖放被拦下;选区消息协议号与 nonce 校验照旧;外层 Esc 先取消选区。
{
  const runtime = source('./htmlPageRuntime.js');
  assert.ok(runtime.includes("window.addEventListener('pointermove'"));
  assert.ok(runtime.includes('MARQUEE_THRESHOLD'));
  const click = runtime.slice(runtime.indexOf("window.addEventListener('click', function (event) {\n    var fromText"));
  assert.ok(click.indexOf('if (suppressClick)') >= 0 && click.indexOf('if (suppressClick)') < click.indexOf('select(pick(event.target))'));
  assert.ok(runtime.includes("['selectstart', 'dragstart']"));
  assert.ok(runtime.includes("case 'clearRegion'"));
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'n', type: 'region' }, 'n').type, 'region');
  assert.equal(htmlPageMessage({ ace_html: HTML_PAGE_PROTOCOL, nonce: 'x', type: 'region' }, 'n'), null);
  const component = source('../components/HtmlPagePreview.jsx');
  const escape = component.slice(component.indexOf("if (key === 'escape') {"));
  assert.ok(escape.indexOf("post({ type: 'clearRegion' })") < escape.indexOf("post({ type: 'select', sid: 0 })"));
  console.log('[pass] HTML edit mode draws a region by dragging anywhere');
}

// 触发场景(回归):先给段落加粗,再双击进去改字,连续按 Ctrl+Z 只能撤回打的字,加粗撤不掉 ——
// 改字期间 Ctrl+Z 走的是浏览器自己的撤销栈,只认这次改字里的输入,撤到头就停,外层那条栈收不到。
// 期望行为:只有一条撤销栈。改字时框里边打边交回外层;改字里的 Ctrl+Z / Ctrl+Y 先把打到一半的字交回,
// 再转给外层;浏览器自己的撤销被拦下;撤到改字里的一步时框内原地换内容、不结束改字;外层收到交回的文字、
// 撤销 / 重做时都不作废这次改字(只有框报 textEditEnded 才作废)。
{
  const runtime = source('./htmlPageRuntime.js');
  const keyBranch = runtime.slice(runtime.indexOf("} else if (mod && !event.altKey && (key === 'z' || key === 'y')) {"));
  assert.ok(keyBranch.length > 0 && keyBranch.indexOf('flushTextCommit(true)') < keyBranch.indexOf('post(keyPayload(event))'));
  assert.ok(runtime.includes("event.inputType === 'historyUndo' || event.inputType === 'historyRedo'"));
  const setInner = runtime.slice(runtime.indexOf("case 'setInner':"), runtime.indexOf("case 'beginTextEdit':"));
  assert.equal(setInner.includes('stopTextEditing'), false);
  assert.equal(setInner.includes("type: 'textEditEnded'"), false);
  assert.ok(setInner.includes('restoreCaret(element, data.caret)'));
  const finish = runtime.slice(runtime.indexOf('function finishTextEdit()'), runtime.indexOf('function finishTextEdit()') + 400);
  assert.ok(finish.indexOf('flushTextCommit(true)') < finish.indexOf('stopTextEditing(element)'));
  const component = source('../components/HtmlPagePreview.jsx');
  const commitText = component.slice(component.indexOf('const commitText = useCallback'), component.indexOf('const removeSelected = useCallback'));
  assert.equal(commitText.includes('pendingTextEditRef.current = 0'), false);
  assert.ok(commitText.includes("mergeKey: merge ? `text:${sid}` : ''"));
  const replay = component.slice(component.indexOf('const replay = useCallback'), component.indexOf('const undo = useCallback'));
  assert.equal(replay.includes('pendingTextEditRef.current = 0'), false);
  console.log('[pass] HTML text editing shares one undo stack with earlier style changes');
}

// 场景:框里交回的光标位置被页面脚本伪造。期望:只认非负整数;结束早于开始或不是整数时按开始算;
// 不是对象、开始为负或超大时作废。
{
  assert.deepEqual(normalizeTextCaret({ start: 3, end: 7 }), { start: 3, end: 7 });
  assert.deepEqual(normalizeTextCaret({ start: 5, end: 2 }), { start: 5, end: 5 });
  assert.deepEqual(normalizeTextCaret({ start: 4 }), { start: 4, end: 4 });
  assert.equal(normalizeTextCaret({ start: -1, end: 2 }), null);
  assert.equal(normalizeTextCaret({ start: 1.5, end: 2 }), null);
  assert.equal(normalizeTextCaret({ start: 1e9 }), null);
  assert.equal(normalizeTextCaret('3'), null);
  console.log('[pass] HTML text caret offsets from the frame are sanitized');
}

// 触发场景:改字时用中文输入法,拼音还没选字就停顿超过 250 毫秒,拼音串被当成正文交回、进了撤销栈,
// 撤销一次段落里留下一串拼音字母。期望行为:组字期间(compositionstart 到 compositionend)不交回,
// 到点的定时器也不交;选完字(compositionend)再排一次交回。
{
  const runtime = source('./htmlPageRuntime.js');
  assert.ok(runtime.includes("document.addEventListener('compositionstart'"));
  const end = runtime.slice(runtime.indexOf("document.addEventListener('compositionend'"));
  assert.ok(end.indexOf('composing = false') >= 0 && end.indexOf('composing = false') < end.indexOf('scheduleTextCommit()'));
  const timer = runtime.slice(runtime.indexOf('function scheduleTextCommit()'), runtime.indexOf('function scheduleTextCommit()') + 300);
  assert.ok(timer.includes('if (!composing) flushTextCommit(true)'));
  console.log('[pass] HTML text editing does not commit unfinished IME composition');
}
