// HTML 文件预览隔离框里的运行时(docs/html-preview.md)。由 htmlPageFrame.js 用 toString
// 拼进 srcdoc,在用户页面的任何脚本之前执行。
//
// 注意:
//   - 整个函数会被序列化进隔离框,不能引用模块里的其它东西,也不能 import;
//   - 不要写中文字符串字面量(构建期文案本地化插件会把它们换成框里不存在的函数调用);
//   - 框是不透明源:读不到外层页面,也调不到本机 /api/*,与外层只靠 postMessage 通信。
//
// 职责:
//   浏览模式 —— 链接与 window.open 交给外层打开,右键 / 快捷键转发给外层,
//               localStorage / sessionStorage / document.cookie 给内存替身(不透明源下一碰就抛错),
//               报告滚动位置,重新渲染后恢复;
//   编辑模式 —— 悬停虚线框、选中实线框(画在 shadow root 里,不受页面样式影响),拦下页面自己的点击,
//               把选中元素的位置、种类与计算样式报给外层;按外层的指令改样式 / 属性 / 删除 / 改字。
//   改字     —— 双击进入,选中框样式不变;框里拖选出的文字报给外层,工具栏的格式只作用在这段文字上
//               (临时切成富文本模式跑浏览器自带的格式命令);边打边交回外层写进源码,和改样式进同一条
//               撤销栈,改字里的 Ctrl+Z / Ctrl+Y 也转给外层,撤到改字里的一步时原地换回内容、继续改字。
//   选区     —— 在页面任意位置按下拖动画出一块矩形选区(按页面坐标记,跟着滚动),把选区和里面的元素
//               报给外层,外层的「AI 编辑」把它作为「选区」引用加进会话。
export function htmlPageRuntime(config) {
  'use strict';
  var SID = 'data-ace-sid';
  var EDIT_ATTR = 'data-ace-text-edit';
  var PROTOCOL = 1;
  var nonce = config.nonce;
  var editing = !!config.editing;
  var hostWindow = window.parent;
  var INLINE = {};
  (config.inlineTags || []).forEach(function (tag) { INLINE[tag] = true; });
  var NON_TEXT = { script: 1, style: 1, textarea: 1, select: 1, iframe: 1, input: 1, img: 1, br: 1, hr: 1, svg: 1, canvas: 1, video: 1, audio: 1 };
  var IMAGE = { img: 1, picture: 1, svg: 1, canvas: 1, video: 1 };
  var RANGE_KEYS = { b: 'bold', i: 'italic', u: 'underline' };
  var SIZE_PLACEHOLDERS = { 'xxx-large': 1, '-webkit-xxx-large': 1 };

  function post(message) {
    message.ace_html = PROTOCOL;
    message.nonce = nonce;
    try { hostWindow.postMessage(message, '*'); } catch (error) { /* 外层已卸载 */ }
  }

  // ---- 不透明源的存储替身 ----------------------------------------------------
  function memoryStorage() {
    var data = Object.create(null);
    return {
      getItem: function (key) { key = String(key); return key in data ? data[key] : null; },
      setItem: function (key, value) { data[String(key)] = String(value); },
      removeItem: function (key) { delete data[String(key)]; },
      clear: function () { data = Object.create(null); },
      key: function (index) { var keys = Object.keys(data); return index < keys.length ? keys[index] : null; },
      get length() { return Object.keys(data).length; },
    };
  }
  ['localStorage', 'sessionStorage'].forEach(function (name) {
    try { void window[name].length; } catch (error) {
      try { Object.defineProperty(window, name, { configurable: true, value: memoryStorage() }); } catch (ignored) { /* 只能放弃 */ }
    }
  });
  try { void document.cookie; } catch (error) {
    try {
      var jar = '';
      Object.defineProperty(document, 'cookie', {
        configurable: true,
        get: function () { return jar; },
        set: function (value) { var pair = String(value).split(';')[0]; jar = jar ? jar + '; ' + pair : pair; },
      });
    } catch (ignored) { /* 只能放弃 */ }
  }

  // ---- 链接 ------------------------------------------------------------------
  var siteBase = String(config.siteBase || '');
  function routeUrl(raw, hashOnly) {
    if (hashOnly) {
      var id = decodeURIComponent(raw.slice(1));
      if (!id || id === 'top') { window.scrollTo(0, 0); return; }
      var target = document.getElementById(id) || document.getElementsByName(id)[0];
      if (target && target.scrollIntoView) target.scrollIntoView();
      return;
    }
    var url;
    try { url = new URL(raw, document.baseURI); } catch (error) { return; }
    if (siteBase && url.href.indexOf(siteBase) === 0) {
      var rest = url.href.slice(siteBase.length).split('#')[0].split('?')[0];
      post({ type: 'openFile', path: rest, hash: url.hash || '' });
      return;
    }
    if (url.protocol === 'http:' || url.protocol === 'https:' || url.protocol === 'mailto:') {
      post({ type: 'link', url: url.href });
    }
  }
  window.addEventListener('click', function (event) {
    if (editing || event.defaultPrevented || event.button !== 0) return;
    var anchor = event.target && event.target.closest ? event.target.closest('a[href]') : null;
    if (!anchor) return;
    var raw = anchor.getAttribute('href') || '';
    if (/^\s*javascript:/i.test(raw)) return;
    event.preventDefault();
    routeUrl(raw, raw.charAt(0) === '#');
  }, false);
  try {
    window.open = function (raw) {
      if (raw) routeUrl(String(raw), String(raw).charAt(0) === '#');
      return null;
    };
  } catch (error) { /* 只读 */ }

  // ---- 快捷键与右键 ------------------------------------------------------------
  function keyPayload(event) {
    return {
      type: 'key', key: event.key, code: event.code, ctrlKey: event.ctrlKey, metaKey: event.metaKey,
      shiftKey: event.shiftKey, altKey: event.altKey, repeat: event.repeat,
    };
  }
  document.addEventListener('keydown', function (event) {
    if (event.isComposing || event.keyCode === 229) return;
    var key = String(event.key || '').toLowerCase();
    var mod = event.ctrlKey || event.metaKey;
    if (textEditing) {
      if (key === 'escape' || (mod && key === 'enter')) {
        event.preventDefault();
        finishTextEdit();
      } else if (mod && key === 's') {
        event.preventDefault();
        finishTextEdit();
        post(keyPayload(event));
      } else if (mod && !event.altKey && (key === 'z' || key === 'y')) {
        // 改字期间的撤销 / 重做也走外层那一条撤销栈(改字前的加粗、改样式都在里面):
        // 先把打到一半的字交回去,再把按键转给外层。
        event.preventDefault();
        flushTextCommit(true);
        post(keyPayload(event));
      } else if (mod && !event.shiftKey && !event.altKey && RANGE_KEYS[key] && editingRange()) {
        // 纯文本编辑模式下浏览器不认 Ctrl+B / I / U,选中了文字时自己补上。
        event.preventDefault();
        runRangeFormat(function () {
          document.execCommand('styleWithCSS', false, false);
          document.execCommand(RANGE_KEYS[key], false, null);
        });
      }
      return;
    }
    if (editing && !mod && !event.altKey && (key === 'escape' || key === 'delete')) {
      event.preventDefault();
      post(keyPayload(event));
      return;
    }
    if (!mod && !event.altKey) return;
    if (!editing && (key === 'z' || key === 'y')) return;
    if (key === 'c' || key === 'x' || key === 'v' || key === 'a') return;
    if (key === 's' || key === 'p' || editing) event.preventDefault();
    post(keyPayload(event));
  }, true);

  if (config.forwardContextMenu) {
    document.addEventListener('contextmenu', function (event) {
      event.preventDefault();
      if (editing && !textEditing) select(pick(event.target));
      var selection = '';
      try { selection = String(window.getSelection() || ''); } catch (error) { selection = ''; }
      post({ type: 'contextmenu', x: event.clientX, y: event.clientY, selection: selection.slice(0, 40000) });
    }, true);
  }

  // ---- 滚动 ------------------------------------------------------------------
  var scrollFrame = 0;
  window.addEventListener('scroll', function () {
    if (!scrollFrame) {
      scrollFrame = requestAnimationFrame(function () {
        scrollFrame = 0;
        post({ type: 'scroll', x: window.scrollX, y: window.scrollY });
        refreshBoxes();
      });
    }
  }, { passive: true, capture: true });
  function restoreScroll() {
    var position = config.scroll;
    if (position && (position.x || position.y)) window.scrollTo(position.x || 0, position.y || 0);
  }

  // ---- 编辑模式:选框 ----------------------------------------------------------
  var overlayHost = null;
  var hoverBox = null;
  var selectBox = null;
  var regionBox = null;
  var hovered = null;
  var selected = null;
  var textEditing = null;
  var textBefore = '';
  var lastPoint = null;
  var removed = {};
  var editStyleReady = false;

  // 改字时浏览器会给获得焦点的可编辑元素画一圈焦点框,和选中框叠在一起像是换了样式:去掉它。
  // 用 adoptedStyleSheets 挂规则,不往页面里插节点;老内核退回 <style>。
  function ensureEditStyle() {
    if (editStyleReady) return;
    editStyleReady = true;
    var css = '[' + EDIT_ATTR + ']:focus,[' + EDIT_ATTR + ']:focus-visible{outline:none!important;}';
    try {
      var sheet = new CSSStyleSheet();
      sheet.replaceSync(css);
      document.adoptedStyleSheets = document.adoptedStyleSheets.concat([sheet]);
    } catch (error) {
      var fallback = document.createElement('style');
      fallback.setAttribute('data-ace-html-runtime', '');
      fallback.textContent = css;
      (document.head || document.documentElement).appendChild(fallback);
    }
  }

  function ensureOverlay() {
    if (overlayHost && overlayHost.isConnected) return;
    overlayHost = document.createElement('ace-html-overlay');
    overlayHost.setAttribute('aria-hidden', 'true');
    overlayHost.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:2147483647;display:block;';
    var root = overlayHost.attachShadow ? overlayHost.attachShadow({ mode: 'closed' }) : overlayHost;
    var style = document.createElement('style');
    style.textContent = '.box{position:fixed;box-sizing:border-box;pointer-events:none;display:none;border-radius:4px;}'
      + '.hover{border:1px dashed ' + config.accent + ';}'
      + '.select{border:2px solid ' + config.accent + ';}'
      + '.region{border:2px solid ' + config.accent + ';border-radius:6px;}'
      + '.region.dragging{border:1px dashed ' + config.accent + ';}'
      + '.region.dragging::before{content:"";position:absolute;inset:0;border-radius:inherit;background:' + config.accent + ';opacity:.1;}';
    hoverBox = document.createElement('div');
    hoverBox.className = 'box hover';
    selectBox = document.createElement('div');
    selectBox.className = 'box select';
    regionBox = document.createElement('div');
    regionBox.className = 'box region';
    root.appendChild(style);
    root.appendChild(hoverBox);
    root.appendChild(selectBox);
    root.appendChild(regionBox);
    document.documentElement.appendChild(overlayHost);
  }

  function place(box, element) {
    if (!box) return;
    if (!element || !element.isConnected || !editing) { box.style.display = 'none'; return; }
    var rect = element.getBoundingClientRect();
    box.style.display = 'block';
    box.style.left = (rect.left - 2) + 'px';
    box.style.top = (rect.top - 2) + 'px';
    box.style.width = (rect.width + 4) + 'px';
    box.style.height = (rect.height + 4) + 'px';
  }

  function rectOf(element) {
    var rect = element.getBoundingClientRect();
    return { x: rect.left, y: rect.top, w: rect.width, h: rect.height, vw: window.innerWidth, vh: window.innerHeight };
  }

  var boxFrame = 0;
  function refreshBoxes() {
    if (!editing || boxFrame) return;
    boxFrame = requestAnimationFrame(function () {
      boxFrame = 0;
      if (selected && !selected.isConnected) {
        selected = null;
        post({ type: 'select', info: null });
      }
      place(hoverBox, hovered !== selected ? hovered : null);
      place(selectBox, selected);
      if (selected) post({ type: 'rect', sid: sidOf(selected), rect: rectOf(selected) });
      placeRegion();
      if (region && !(marquee && marquee.active)) post({ type: 'regionRect', rect: regionClientRect() });
    });
  }
  window.addEventListener('resize', refreshBoxes);

  function pick(node) {
    var element = node && node.nodeType === 1 ? node : node && node.parentElement;
    if (!element || element === overlayHost) return null;
    if (element === document.documentElement || element === document.body) return null;
    return element;
  }

  function sidOf(element) {
    var value = element && element.getAttribute ? Number(element.getAttribute(SID)) : 0;
    return Number.isFinite(value) && value > 0 ? value : 0;
  }

  function bySid(sid) {
    if (!sid) return null;
    return document.querySelector('[' + SID + '="' + Number(sid) + '"]');
  }

  function isTextLeaf(element) {
    var tag = element.tagName.toLowerCase();
    if (NON_TEXT[tag] || !/\S/.test(element.textContent || '')) return false;
    var all = element.getElementsByTagName('*');
    for (var index = 0; index < all.length; index += 1) {
      if (!INLINE[all[index].tagName.toLowerCase()]) return false;
    }
    return true;
  }

  function cssPath(element) {
    var parts = [];
    for (var node = element; node && node.nodeType === 1 && node !== document.documentElement; node = node.parentElement) {
      var part = node.tagName.toLowerCase();
      if (node.id) { parts.unshift(part + '#' + node.id); break; }
      var parent = node.parentElement;
      if (parent) {
        var same = Array.prototype.filter.call(parent.children, function (child) { return child.tagName === node.tagName; });
        if (same.length > 1) part += ':nth-of-type(' + (same.indexOf(node) + 1) + ')';
      }
      parts.unshift(part);
    }
    return parts.join(' > ');
  }

  function cleanClone(element) {
    var clone = element.cloneNode(true);
    clone.removeAttribute(SID);
    clone.removeAttribute('contenteditable');
    clone.removeAttribute(EDIT_ATTR);
    var marked = clone.querySelectorAll('[' + SID + '],[contenteditable],[' + EDIT_ATTR + ']');
    for (var index = 0; index < marked.length; index += 1) {
      marked[index].removeAttribute(SID);
      marked[index].removeAttribute('contenteditable');
      marked[index].removeAttribute(EDIT_ATTR);
    }
    return clone;
  }

  function describe(element) {
    var tag = element.tagName.toLowerCase();
    var sid = sidOf(element);
    var computed = getComputedStyle(element);
    var textLeaf = isTextLeaf(element);
    return {
      sid: sid,
      duplicate: !!sid && document.querySelectorAll('[' + SID + '="' + sid + '"]').length > 1,
      tag: tag,
      kind: IMAGE[tag] ? 'image' : (textLeaf ? 'text' : 'block'),
      textLeaf: textLeaf,
      rect: rectOf(element),
      text: String(element.innerText || element.textContent || '').trim().slice(0, 400),
      selector: cssPath(element),
      outerHTML: cleanClone(element).outerHTML.slice(0, 8000),
      href: tag === 'a' ? (element.getAttribute('href') || '') : '',
      src: tag === 'img' ? (element.getAttribute('src') || '') : '',
      styles: {
        fontSize: computed.fontSize,
        fontWeight: computed.fontWeight,
        fontStyle: computed.fontStyle,
        textDecorationLine: computed.textDecorationLine,
        textAlign: computed.textAlign,
        color: computed.color,
        backgroundColor: computed.backgroundColor,
        borderColor: computed.borderTopColor,
        borderWidth: computed.borderTopWidth,
        borderStyle: computed.borderTopStyle,
        borderRadius: computed.borderTopLeftRadius,
      },
    };
  }

  function select(element) {
    if (textEditing && element !== textEditing) finishTextEdit();
    if (element && region) clearRegion(true);
    selected = element || null;
    refreshBoxes();
    post({ type: 'select', info: selected ? describe(selected) : null });
  }

  function report() {
    if (selected && selected.isConnected) post({ type: 'select', info: describe(selected) });
  }

  // ---- 编辑模式:拦下页面自己的交互 ----------------------------------------------
  function insideText(event) {
    return textEditing && event.target && textEditing.contains(event.target);
  }
  function swallow(event) {
    if (!editing || insideText(event)) return false;
    event.preventDefault();
    event.stopImmediatePropagation();
    return true;
  }
  // 在改字元素里按下、拖选时拖过头在外面松开:浏览器把这次 click 派给按下与松开处的共同祖先,
  // 它不算「点了别处」,只吞掉,不结束改字、不换选中。
  var textPointer = false;
  ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'auxclick', 'touchstart', 'submit'].forEach(function (type) {
    window.addEventListener(type, function (event) {
      if (!editing) return;
      if (type === 'pointerdown') {
        textPointer = !!(textEditing && insideText(event));
        if (textEditing && !textPointer) finishTextEdit();
        beginMarquee(event);
      } else if (type === 'pointerup') {
        endMarquee();
      }
      swallow(event);
    }, { capture: true, passive: false });
  });
  // 编辑模式里页面自己的文字选择与拖放都不要(选元素靠单击、画选区靠拖动);改字的元素里照常。
  ['selectstart', 'dragstart'].forEach(function (type) {
    document.addEventListener(type, function (event) {
      if (editing && !insideText(event)) event.preventDefault();
    }, true);
  });
  window.addEventListener('click', function (event) {
    var fromText = textPointer;
    textPointer = false;
    if (fromText && textEditing) {
      if (!insideText(event)) swallow(event);
      return;
    }
    if (suppressClick) {
      // 刚画完选区松开鼠标:这次 click 不是选元素。
      suppressClick = false;
      swallow(event);
      return;
    }
    if (!swallow(event)) return;
    lastPoint = { x: event.clientX, y: event.clientY };
    // 按下时拦了默认行为,框不会自己拿到焦点;不拿焦点的话 Delete / Esc / 撤销都落到外层页面。
    try { window.focus(); } catch (error) { /* 拿不到焦点时快捷键由外层兜底 */ }
    select(pick(event.target));
  }, true);
  window.addEventListener('dblclick', function (event) {
    if (!swallow(event)) return;
    lastPoint = { x: event.clientX, y: event.clientY };
    var element = textBlockOf(pick(event.target));
    if (element !== selected) select(element);
    if (element) post({ type: 'textEditRequest', sid: sidOf(element) });
  }, true);
  // 双击段落里加粗 / 带链接的几个字,进入的是整段文字,而不是里面那个 <b> / <a>(单击仍可选中它单独改样式)。
  function textBlockOf(element) {
    var node = element;
    while (node && INLINE[node.tagName.toLowerCase()]) {
      var parent = node.parentElement;
      if (!parent || parent === document.body || parent === document.documentElement || !sidOf(parent) || !isTextLeaf(parent)) break;
      node = parent;
    }
    return node;
  }

  // ---- 编辑模式:拖出一块选区 ----------------------------------------------------
  // 在页面任意位置按下拖动(超过几个像素)画出一块矩形选区,松开后留在页面上(按页面坐标记,跟着滚动),
  // 把选区和里面的元素报给外层;外层的「AI 编辑」把它作为「选区」引用加进会话。
  var MARQUEE_THRESHOLD = 5;
  var REGION_MIN_SIZE = 6;
  var REGION_ELEMENT_LIMIT = 30;
  var SKIP_REGION = { script: 1, style: 1, template: 1, noscript: 1, link: 1, meta: 1, br: 1, wbr: 1 };
  var marquee = null;
  var region = null;
  var suppressClick = false;

  // 按在滚动条上(页面自己的或某个可滚动容器的)是在拖滚动条,不画选区。
  function onScrollbar(event) {
    var root = document.documentElement;
    if (event.clientX >= root.clientWidth || event.clientY >= root.clientHeight) return true;
    var element = event.target && event.target.nodeType === 1 ? event.target : null;
    if (!element || element === root || element === document.body) return false;
    var box = element.getBoundingClientRect();
    var right = box.left + element.clientLeft + element.clientWidth;
    var bottom = box.top + element.clientTop + element.clientHeight;
    return (element.offsetWidth - element.clientWidth > element.clientLeft * 2 && event.clientX >= right)
      || (element.offsetHeight - element.clientHeight > element.clientTop * 2 && event.clientY >= bottom);
  }

  function beginMarquee(event) {
    suppressClick = false;
    marquee = !textPointer && event.button === 0 && event.isPrimary !== false && !onScrollbar(event)
      ? { x: event.clientX + window.scrollX, y: event.clientY + window.scrollY, active: false, sized: false }
      : null;
  }

  window.addEventListener('pointermove', function (event) {
    if (!editing || !marquee) return;
    if (!(event.buttons & 1)) {
      // 松开时没收到 pointerup(比如在框外松开):按已松开收尾。
      if (marquee.active) endMarquee();
      else marquee = null;
      return;
    }
    var x = event.clientX + window.scrollX;
    var y = event.clientY + window.scrollY;
    if (!marquee.active) {
      if (Math.abs(x - marquee.x) + Math.abs(y - marquee.y) < MARQUEE_THRESHOLD) return;
      marquee.active = true;
      // 按下事件被拦了默认行为,拖出框后浏览器不再把移动 / 松开交给框:让外层在它那边等这次松开,
      // 松开时发 endMarquee 回来收尾(选区停在最后一次在框里的位置)。
      post({ type: 'marquee' });
      if (region) clearRegion(true);
      hovered = null;
    }
    // 画选区期间的移动不交给页面自己的拖动逻辑。
    event.stopImmediatePropagation();
    region = {
      left: Math.min(marquee.x, x),
      top: Math.min(marquee.y, y),
      width: Math.abs(x - marquee.x),
      height: Math.abs(y - marquee.y),
    };
    // 选区真的画出来了(宽高都够)才取消元素选中;手抖拖了几像素的单击不该丢掉选中。
    if (!marquee.sized && region.width >= REGION_MIN_SIZE && region.height >= REGION_MIN_SIZE) {
      marquee.sized = true;
      if (selected) select(null);
    }
    refreshBoxes();
  }, true);
  window.addEventListener('pointercancel', function () {
    if (marquee && marquee.active) endMarquee();
    marquee = null;
  }, true);

  function endMarquee() {
    var drawn = marquee && marquee.active;
    marquee = null;
    if (!drawn) return;
    if (!region || region.width < REGION_MIN_SIZE || region.height < REGION_MIN_SIZE) {
      // 太小,不算选区:这次 click 照常选中元素。
      clearRegion(false);
      refreshBoxes();
      return;
    }
    suppressClick = true;
    // 和单击选中一样让框拿到焦点,Esc 才能取消选区。
    try { window.focus(); } catch (error) { /* 拿不到焦点时由外层兜底 */ }
    refreshBoxes();
    post({ type: 'region', region: describeRegion() });
  }

  function regionClientRect() {
    if (!region) return null;
    var x = region.left - window.scrollX;
    var y = region.top - window.scrollY;
    return { x: x, y: y, w: region.width, h: region.height, vw: window.innerWidth, vh: window.innerHeight };
  }

  function placeRegion() {
    if (!regionBox) return;
    var rect = editing ? regionClientRect() : null;
    if (!rect) { regionBox.style.display = 'none'; return; }
    regionBox.classList.toggle('dragging', !!(marquee && marquee.active));
    regionBox.style.display = 'block';
    regionBox.style.left = rect.x + 'px';
    regionBox.style.top = rect.y + 'px';
    regionBox.style.width = rect.w + 'px';
    regionBox.style.height = rect.h + 'px';
  }

  function clearRegion(notify) {
    if (!region) return;
    region = null;
    placeRegion();
    if (notify) post({ type: 'region', region: null });
  }

  function within(outer, inner, slack) {
    return inner.left >= outer.left - slack && inner.top >= outer.top - slack
      && inner.right <= outer.right + slack && inner.bottom <= outer.bottom + slack;
  }

  function overlaps(a, b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
  }

  // 文字元素(标题、段落)的盒子常常横跨整行,看得见的文字只占一小段:按文字实际占的范围算。
  function contentBox(element) {
    if (!isTextLeaf(element)) return null;
    try {
      var range = document.createRange();
      range.selectNodeContents(element);
      var box = range.getBoundingClientRect();
      return box.width >= 1 && box.height >= 1 ? box : null;
    } catch (error) {
      return null;
    }
  }

  function overlapRatio(rect, box) {
    var width = Math.min(rect.right, box.right) - Math.max(rect.left, box.left);
    var height = Math.min(rect.bottom, box.bottom) - Math.max(rect.top, box.top);
    if (width <= 0 || height <= 0) return 0;
    return (width * height) / Math.max(1, box.width * box.height);
  }

  // 选区里有哪些元素:完全落在选区里的最外层元素,加上有四分之一以上落在选区里的文字 / 图片元素
  // (标成只有部分在选区里);一个都没有时取包住整个选区的最小元素。最多 30 个。
  function regionElements(rect) {
    var body = document.body;
    if (!body) return [];
    var all = body.getElementsByTagName('*');
    var inside = [];
    var touched = [];
    var picked = typeof Set === 'function' ? new Set() : null;
    function pickedAncestor(element) {
      for (var node = element.parentElement; node && node !== body; node = node.parentElement) {
        if (picked && picked.has(node)) return true;
      }
      return false;
    }
    for (var index = 0; index < all.length && inside.length < REGION_ELEMENT_LIMIT; index += 1) {
      var element = all[index];
      if (SKIP_REGION[element.tagName.toLowerCase()] || pickedAncestor(element)) continue;
      var box = element.getBoundingClientRect();
      if (box.width < 1 || box.height < 1 || !overlaps(rect, box)) continue;
      var text = within(rect, box, 2) ? null : contentBox(element);
      if (within(rect, box, 2) || (text && within(rect, text, 2))) {
        inside.push(element);
        if (picked) picked.add(element);
      } else if (touched.length < REGION_ELEMENT_LIMIT
          && (IMAGE[element.tagName.toLowerCase()] || text)
          && overlapRatio(rect, text || box) >= 0.25) {
        touched.push(element);
      }
    }
    // 部分相交的:去掉包住了完全落在选区里的元素的(那个元素更准),也去掉被别的相交元素包住的。
    var partial = touched.filter(function (item) {
      if (inside.some(function (other) { return item.contains(other) || other.contains(item); })) return false;
      return !touched.some(function (other) { return other !== item && other.contains(item); });
    });
    var result = inside.map(function (item) { return describeRegionElement(item, 'inside'); })
      .concat(partial.map(function (item) { return describeRegionElement(item, 'partial'); }))
      .slice(0, REGION_ELEMENT_LIMIT);
    if (result.length) return result;
    var around = pick(document.elementFromPoint(rect.left + rect.width / 2, rect.top + rect.height / 2));
    while (around && !within(around.getBoundingClientRect(), rect, 0)) around = pick(around.parentElement);
    return around ? [describeRegionElement(around, 'container')] : [];
  }

  // relation:inside = 整个在选区里;partial = 只有一部分在选区里;container = 选区落在它里面。
  function describeRegionElement(element, relation) {
    var sid = sidOf(element);
    // 脚本生成的元素:带上离它最近、源码里有的祖先,模型能顺着找到往里填内容的脚本。
    var anchor = 0;
    for (var node = sid ? null : element.parentElement; node && node !== document.body; node = node.parentElement) {
      if (sidOf(node)) { anchor = sidOf(node); break; }
    }
    return {
      sid: sid,
      anchorSid: anchor,
      duplicate: !!sid && document.querySelectorAll('[' + SID + '="' + sid + '"]').length > 1,
      tag: element.tagName.toLowerCase(),
      selector: cssPath(element),
      text: String(element.innerText || element.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 200),
      html: sid ? '' : cleanClone(element).outerHTML.slice(0, 3000),
      relation: relation,
    };
  }

  function describeRegion() {
    var rect = regionClientRect();
    var clientBox = { left: rect.x, top: rect.y, right: rect.x + rect.w, bottom: rect.y + rect.h, width: rect.w, height: rect.h };
    return {
      rect: rect,
      page: { x: region.left, y: region.top, w: region.width, h: region.height },
      scroll: { x: window.scrollX, y: window.scrollY },
      elements: regionElements(clientBox),
    };
  }

  window.addEventListener('mousemove', function (event) {
    if (!editing) return;
    var element = pick(event.target);
    // 正在改字的元素里拖选文字时,不给里面的 <b> / <span>、也不给拖过头经过的元素画悬停框。
    if (element && textEditing && (textEditing.contains(element) || (textPointer && event.buttons))) element = null;
    if (element === hovered) return;
    hovered = element;
    refreshBoxes();
  }, true);
  document.documentElement.addEventListener('mouseleave', function () {
    hovered = null;
    refreshBoxes();
  });
  if (typeof MutationObserver === 'function') {
    new MutationObserver(function () { refreshBoxes(); }).observe(document.documentElement, {
      subtree: true, childList: true, attributes: true, characterData: true,
    });
  }

  // ---- 改字 ------------------------------------------------------------------
  function normalizedInner(html) {
    var template = document.createElement('template');
    template.innerHTML = html;
    var holder = document.createElement('div');
    holder.appendChild(template.content.cloneNode(true));
    return holder.innerHTML;
  }

  function beginTextEdit(sid, expected) {
    var element = bySid(sid);
    if (!element) return post({ type: 'textEditRejected', sid: sid, reason: 'missing' });
    var current = cleanClone(element).innerHTML;
    if (typeof expected === 'string' && normalizedInner(expected) !== current) {
      return post({ type: 'textEditRejected', sid: sid, reason: 'script' });
    }
    if (selected !== element) select(element);
    ensureEditStyle();
    textEditing = element;
    textBefore = current;
    element.setAttribute(EDIT_ATTR, '');
    try { element.contentEditable = 'plaintext-only'; } catch (error) { element.contentEditable = 'true'; }
    if (element.contentEditable !== 'plaintext-only') element.contentEditable = 'true';
    element.spellcheck = false;
    element.focus();
    try {
      var range = lastPoint && document.caretRangeFromPoint ? document.caretRangeFromPoint(lastPoint.x, lastPoint.y) : null;
      if (range && element.contains(range.startContainer)) {
        var selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
      }
    } catch (error) { /* 光标停在开头即可 */ }
    committedCaret = caretOffsets(element);
    chunkCaret = null;
    post({ type: 'textEditStarted', sid: sid });
  }

  function stopTextEditing(element) {
    textEditing = null;
    rangeSaved = null;
    lastRangeReport = '';
    if (rangeFrame) cancelAnimationFrame(rangeFrame);
    rangeFrame = 0;
    if (textCommitTimer) clearTimeout(textCommitTimer);
    textCommitTimer = 0;
    chunkCaret = null;
    committedCaret = null;
    element.removeAttribute('contenteditable');
    element.removeAttribute('spellcheck');
    element.removeAttribute(EDIT_ATTR);
  }

  function finishTextEdit() {
    var element = textEditing;
    if (!element) return;
    // 先交文字再报结束:外层只接受自己发起的这次改字,收到结束就作废。
    flushTextCommit(true);
    stopTextEditing(element);
    post({ type: 'textEditEnded', sid: sidOf(element) });
    try { window.getSelection().removeAllRanges(); } catch (error) { /* 无选区 */ }
    report();
  }

  // ---- 改字:边打边交回外层 ------------------------------------------------------
  // 改字不再是离开时才整段交回:停顿 TEXT_COMMIT_DELAY 毫秒就交一次,和改样式、加粗进外层同一条
  // 撤销栈(外层把连续打字合成一步)。浏览器自己的撤销栈不用(它只认这次改字里的输入,撤到头就断了)。
  // 每次交回带上这一步前后的光标(按字符数算),撤销 / 重做原地换回内容时把光标放回去。
  var TEXT_COMMIT_DELAY = 250;
  var textCommitTimer = 0;
  var chunkCaret = null;
  var committedCaret = null;

  function textOffsetIn(root, node, offset) {
    var range = document.createRange();
    range.selectNodeContents(root);
    range.setEnd(node, offset);
    return range.toString().length;
  }

  function caretOffsets(element) {
    try {
      var selection = window.getSelection();
      if (!element || !selection || !selection.rangeCount) return null;
      var range = selection.getRangeAt(0);
      if (!element.contains(range.startContainer) || !element.contains(range.endContainer)) return null;
      return {
        start: textOffsetIn(element, range.startContainer, range.startOffset),
        end: textOffsetIn(element, range.endContainer, range.endOffset),
      };
    } catch (error) {
      return null;
    }
  }

  function pointAt(root, offset) {
    var walker = document.createTreeWalker(root, 4);
    var rest = Math.max(0, offset);
    var last = null;
    for (var node = walker.nextNode(); node; node = walker.nextNode()) {
      last = node;
      if (rest <= node.data.length) return { node: node, offset: rest };
      rest -= node.data.length;
    }
    return last ? { node: last, offset: last.data.length } : { node: root, offset: root.childNodes.length };
  }

  // 没给光标时放到末尾。
  function restoreCaret(element, caret) {
    var wanted = caret && typeof caret.start === 'number' ? caret : null;
    try {
      var start = pointAt(element, wanted ? wanted.start : Infinity);
      var end = wanted && typeof wanted.end === 'number' ? pointAt(element, wanted.end) : start;
      var range = document.createRange();
      range.setStart(start.node, start.offset);
      range.setEnd(end.node, end.offset);
      var selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
    } catch (error) {
      return null;
    }
    return caretOffsets(element);
  }

  // 输入法组字期间(拼音还没选字)不交回,否则拼音串会被当成正文进撤销栈;选完字再交。
  var composing = false;

  function scheduleTextCommit() {
    if (textCommitTimer) clearTimeout(textCommitTimer);
    textCommitTimer = setTimeout(function () {
      textCommitTimer = 0;
      if (!composing) flushTextCommit(true);
    }, TEXT_COMMIT_DELAY);
  }

  // 外层直接改了正在改字的元素的内容(只有光标时加 / 去链接):外层已把这一步写进源码。之前没有
  // 没交的字就只更新「已交回的内容」,不多出一步;有没交的字就连同改动一起交回(不能丢字)。
  function editingDirty(element) {
    return textEditing === element && cleanClone(element).innerHTML !== textBefore;
  }

  function afterOuterInnerChange(element, dirtyBefore) {
    if (textEditing !== element) return;
    if (dirtyBefore) flushTextCommit(true);
    else textBefore = cleanClone(element).innerHTML;
  }

  // merge:能否和上一步连续打字合成一步(格式命令是单独一步)。
  function flushTextCommit(merge) {
    if (textCommitTimer) clearTimeout(textCommitTimer);
    textCommitTimer = 0;
    var element = textEditing;
    if (!element) return;
    var html = cleanClone(element).innerHTML;
    var before = chunkCaret || committedCaret;
    chunkCaret = null;
    if (html === textBefore) return;
    var after = caretOffsets(element);
    textBefore = html;
    committedCaret = after;
    post({ type: 'textCommit', sid: sidOf(element), html: html, caretBefore: before, caretAfter: after, merge: !!merge });
  }

  document.addEventListener('beforeinput', function (event) {
    if (!textEditing || !event.target || !textEditing.contains(event.target)) return;
    if (event.inputType === 'historyUndo' || event.inputType === 'historyRedo') {
      event.preventDefault();
      return;
    }
    if (!chunkCaret) chunkCaret = caretOffsets(textEditing);
  }, true);
  document.addEventListener('input', function (event) {
    if (!textEditing || !event.target || !textEditing.contains(event.target)) return;
    if (composing || event.isComposing) return;
    scheduleTextCommit();
  }, true);
  document.addEventListener('compositionstart', function () {
    composing = true;
    if (textEditing && !chunkCaret) chunkCaret = caretOffsets(textEditing);
  }, true);
  document.addEventListener('compositionend', function () {
    composing = false;
    if (textEditing) scheduleTextCommit();
  }, true);

  // ---- 改字:选中的一段文字 ------------------------------------------------------
  // 外层工具栏点按钮时焦点留在框里;点进字号 / 地址输入框时框会失焦,所以记下最后一次的选区,
  // 执行前放回去。
  var rangeSaved = null;
  var rangeFrame = 0;
  var lastRangeReport = '';

  function editingRange() {
    if (!textEditing) return null;
    var selection;
    try { selection = window.getSelection(); } catch (error) { return null; }
    if (!selection || !selection.rangeCount) return null;
    var range = selection.getRangeAt(0);
    if (range.collapsed || !textEditing.contains(range.commonAncestorContainer)) return null;
    return rangeTextNodes(range).length ? range : null;
  }

  function rangeTextNodes(range) {
    var root = range.commonAncestorContainer;
    if (root.nodeType === 3) return /\S/.test(range.toString()) ? [root] : [];
    var nodes = [];
    var walker = document.createTreeWalker(root, 4);
    for (var node = walker.nextNode(); node; node = walker.nextNode()) {
      if (!range.intersectsNode(node)) continue;
      var start = node === range.startContainer ? range.startOffset : 0;
      var end = node === range.endContainer ? range.endOffset : node.data.length;
      if (/\S/.test(node.data.slice(start, end))) nodes.push(node);
    }
    return nodes;
  }

  // 下划线 / 删除线不继承到子元素的计算样式里,要沿祖先找。
  function hasLine(element, line) {
    for (var node = element; node && node.nodeType === 1; node = node.parentElement) {
      if (String(getComputedStyle(node).textDecorationLine || '').indexOf(line) >= 0) return true;
    }
    return false;
  }

  function describeRange(range) {
    var nodes = rangeTextNodes(range);
    if (!nodes.length) return null;
    function all(test) {
      return nodes.every(function (node) { return test(node.parentElement); });
    }
    var first = nodes[0].parentElement;
    var computed = getComputedStyle(first);
    var bold = all(function (element) {
      var weight = getComputedStyle(element).fontWeight;
      return weight === 'bold' || weight === 'bolder' || Number(weight) >= 600;
    });
    var italic = all(function (element) { return getComputedStyle(element).fontStyle !== 'normal'; });
    var lines = [];
    if (all(function (element) { return hasLine(element, 'underline'); })) lines.push('underline');
    if (all(function (element) { return hasLine(element, 'line-through'); })) lines.push('line-through');
    var anchor = first.closest('a[href]');
    if (anchor && anchor !== textEditing && !textEditing.contains(anchor)) anchor = null;
    return {
      text: range.toString().slice(0, 400),
      href: anchor ? anchor.getAttribute('href') || '' : '',
      styles: {
        fontSize: computed.fontSize,
        color: computed.color,
        fontWeight: bold ? '700' : '400',
        fontStyle: italic ? 'italic' : 'normal',
        textDecorationLine: lines.length ? lines.join(' ') : 'none',
      },
    };
  }

  function reportTextSelection() {
    if (rangeFrame) cancelAnimationFrame(rangeFrame);
    rangeFrame = 0;
    if (!textEditing) return;
    var range = editingRange();
    var info = range ? describeRange(range) : null;
    if (info) rangeSaved = range.cloneRange();
    else if (hasCaret()) rangeSaved = null;
    var message = { type: 'textSelection', sid: sidOf(textEditing), range: info };
    var key = JSON.stringify(info);
    if (key === lastRangeReport) return;
    lastRangeReport = key;
    post(message);
  }

  // 框失焦时选区还在;只有光标真的落回框里(折叠)才算取消了选中。
  function hasCaret() {
    try {
      var selection = window.getSelection();
      return !!selection && selection.rangeCount > 0 && selection.getRangeAt(0).collapsed
        && textEditing.contains(selection.getRangeAt(0).startContainer);
    } catch (error) {
      return false;
    }
  }

  document.addEventListener('selectionchange', function () {
    if (!textEditing || rangeFrame) return;
    rangeFrame = requestAnimationFrame(reportTextSelection);
  });

  // 浏览器的格式命令只在富文本编辑里可用:执行期间临时切过去,结束马上切回纯文本(打字、粘贴、
  // 回车仍按纯文本处理,不会冒出 <div>)。
  function withRichEditing(run) {
    var host = textEditing;
    var previous = host.getAttribute('contenteditable');
    if (previous !== 'true') host.setAttribute('contenteditable', 'true');
    try { run(); } finally { if (previous !== 'true') host.setAttribute('contenteditable', previous); }
  }

  function richCommand(command, value, css) {
    document.execCommand('styleWithCSS', false, !!css);
    return document.execCommand(command, false, value);
  }

  function toggleTo(command, wanted) {
    document.execCommand('styleWithCSS', false, false);
    if (document.queryCommandState(command) !== wanted) richCommand(command, null, false);
  }

  // 字号命令只认 1-7 档:先套最大一档,再把选区里这一档的样式换成要的像素值。
  function applyFontSize(size) {
    richCommand('fontSize', '7', true);
    var range = editingRange();
    var candidates = [textEditing].concat(Array.prototype.slice.call(textEditing.querySelectorAll('[style]')));
    candidates.forEach(function (node) {
      if (range && !range.intersectsNode(node)) return;
      if (!SIZE_PLACEHOLDERS[node.style.fontSize]) return;
      node.style.fontSize = size;
    });
  }

  // 一次选区格式:先把打到一半的字交成一步,格式本身再单独交一步(撤销时先撤格式、再撤打字)。
  function runRangeFormat(run) {
    flushTextCommit(true);
    chunkCaret = caretOffsets(textEditing);
    withRichEditing(run);
    flushTextCommit(false);
    afterRangeFormat();
  }

  function isBold(value) {
    var text = String(value || '').toLowerCase();
    return text === 'bold' || text === 'bolder' || Number(text) >= 600;
  }

  function afterRangeFormat() {
    lastRangeReport = '';
    reportTextSelection();
    report();
  }

  function formatRange(data) {
    if (!textEditing) return;
    var range = editingRange();
    if (!range && rangeSaved && textEditing.contains(rangeSaved.commonAncestorContainer) && !rangeSaved.collapsed) {
      range = rangeSaved;
      var selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
    }
    if (!range) return;
    var styles = data.styles || {};
    runRangeFormat(function () {
      if ('font-weight' in styles) toggleTo('bold', isBold(styles['font-weight']));
      if ('font-style' in styles) toggleTo('italic', String(styles['font-style'] || 'normal') !== 'normal');
      if ('text-decoration-line' in styles) {
        var lines = String(styles['text-decoration-line'] || '');
        toggleTo('underline', lines.indexOf('underline') >= 0);
        toggleTo('strikeThrough', lines.indexOf('line-through') >= 0);
      }
      if (styles.color) richCommand('foreColor', String(styles.color), true);
      if (styles['font-size']) applyFontSize(String(styles['font-size']));
      if (typeof data.link === 'string') {
        if (data.link) richCommand('createLink', data.link, false);
        else richCommand('unlink', null, false);
      }
    });
  }
  document.addEventListener('paste', function (event) {
    if (!textEditing || textEditing.contentEditable === 'plaintext-only') return;
    event.preventDefault();
    var text = event.clipboardData ? event.clipboardData.getData('text/plain') : '';
    document.execCommand('insertText', false, text);
  }, true);

  // ---- 外层指令 ----------------------------------------------------------------
  function kebab(name) {
    return String(name).replace(/[A-Z]/g, function (ch) { return '-' + ch.toLowerCase(); });
  }

  window.addEventListener('message', function (event) {
    if (event.source !== hostWindow) return;
    var data = event.data;
    if (!data || data.ace_html !== PROTOCOL || data.nonce !== nonce) return;
    var element = data.sid ? bySid(data.sid) : null;
    switch (data.type) {
      case 'mode':
        editing = !!data.editing;
        if (!editing) {
          finishTextEdit();
          hovered = null;
          selected = null;
          marquee = null;
          region = null;
        } else {
          ensureOverlay();
        }
        refreshBoxes();
        place(hoverBox, null);
        place(selectBox, selected);
        placeRegion();
        break;
      case 'clearRegion':
        clearRegion(true);
        break;
      case 'endMarquee':
        if (marquee && marquee.active) endMarquee();
        break;
      case 'select':
        select(element);
        break;
      case 'patchStyle':
        if (!element) break;
        Object.keys(data.styles || {}).forEach(function (name) {
          var value = data.styles[name];
          if (value === null || value === undefined || value === '') element.style.removeProperty(kebab(name));
          else element.style.setProperty(kebab(name), String(value));
        });
        if (!element.getAttribute('style')) element.removeAttribute('style');
        report();
        refreshBoxes();
        break;
      case 'setAttribute':
        if (!element) break;
        if (data.value === null || data.value === undefined) element.removeAttribute(data.name);
        else element.setAttribute(data.name, String(data.value));
        report();
        refreshBoxes();
        break;
      case 'wrapLink':
        if (!element) break;
        var linkDirty = editingDirty(element);
        var anchor = document.createElement('a');
        anchor.setAttribute('href', String(data.href || ''));
        while (element.firstChild) anchor.appendChild(element.firstChild);
        element.appendChild(anchor);
        afterOuterInnerChange(element, linkDirty);
        report();
        break;
      case 'unwrapLink':
        // 撤销「加链接」:去掉 wrapLink 包上的那一层 <a>,内容原样留下。
        if (!element) break;
        var unlinkDirty = editingDirty(element);
        var wrapper = element.firstElementChild;
        if (wrapper && wrapper.tagName === 'A' && element.childNodes.length === 1) {
          while (wrapper.firstChild) element.insertBefore(wrapper.firstChild, wrapper);
          wrapper.remove();
        }
        afterOuterInnerChange(element, unlinkDirty);
        report();
        break;
      case 'remove':
        if (!element) break;
        if (textEditing && (element === textEditing || element.contains(textEditing))) finishTextEdit();
        if (element === selected) selected = null;
        if (element === hovered) hovered = null;
        // 留着节点本身,撤销删除时原样插回(脚本状态、子元素编号都还在)。
        removed[data.sid] = { node: element, parent: element.parentNode, next: element.nextSibling };
        element.remove();
        post({ type: 'select', info: null });
        refreshBoxes();
        break;
      case 'restore':
        var stash = removed[data.sid];
        if (!stash || !stash.parent || !stash.parent.isConnected) break;
        stash.parent.insertBefore(stash.node, stash.next && stash.next.parentNode === stash.parent ? stash.next : null);
        delete removed[data.sid];
        refreshBoxes();
        break;
      case 'setInner':
        // 撤销 / 重做改字:按外层给的(带编号的)源码片段原地换掉内容,不重新加载页面。
        if (!element || typeof data.html !== 'string') break;
        if (textEditing && textEditing !== element && element.contains(textEditing)) finishTextEdit();
        element.innerHTML = data.html;
        if (textEditing === element) {
          // 改字中撤销 / 重做:原地换内容,继续改字,光标放回那一步的位置。
          if (textCommitTimer) clearTimeout(textCommitTimer);
          textCommitTimer = 0;
          textBefore = cleanClone(element).innerHTML;
          chunkCaret = null;
          committedCaret = restoreCaret(element, data.caret);
          lastRangeReport = '';
          reportTextSelection();
        }
        report();
        refreshBoxes();
        break;
      case 'beginTextEdit':
        beginTextEdit(data.sid, data.expected);
        break;
      case 'formatRange':
        formatRange(data);
        break;
      case 'endTextEdit':
        finishTextEdit();
        break;
      case 'assignSids':
        if (!element) break;
        var descendants = element.getElementsByTagName('*');
        var ids = data.ids || [];
        for (var index = 0; index < descendants.length; index += 1) {
          if (descendants.length === ids.length) descendants[index].setAttribute(SID, String(ids[index]));
          else descendants[index].removeAttribute(SID);
        }
        break;
      default:
        break;
    }
  });

  // ---- 启动 ------------------------------------------------------------------
  function ready() {
    if (editing) ensureOverlay();
    restoreScroll();
    if (config.selectSid) {
      var element = bySid(config.selectSid);
      if (element) select(element);
    }
    post({ type: 'ready' });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ready);
  else ready();
  window.addEventListener('load', function () {
    restoreScroll();
    refreshBoxes();
  });
}
