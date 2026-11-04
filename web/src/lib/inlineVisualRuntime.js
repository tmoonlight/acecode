// 对话内可视化隔离框的「框内运行时」(openspec add-inline-visuals,docs/inline-visuals.md)。
//
// 整个运行时是一个不引用外部变量的函数:inlineVisualFrame.js 用 Function.prototype.toString
// 把它拼进 srcdoc,在隔离框里以 (inlineVisualRuntime)(config) 启动。所以:
//   - 函数体里不能用 import 进来的任何东西,也不能引用本模块的其它顶层声明;
//   - 只写不需要编译辅助函数的语法(不用对象展开、class 字段等);
//   - 不写中文字符串字面量(构建期文案本地化插件会把它们换成 tr(),框里没有 tr)。
// 第二个参数 host 只给 Node 测试用:注入假 DOM 的 document / 解析函数 / 根节点,
// 并可关掉启动时的全局安装(boot:false),返回内部函数供测试调用。
//
// 职责:
//   - 流式更新:每次先剥掉 <script>(含末尾写了一半的)与 on* 属性,解析后按位置差量对齐到
//     #ace-root,已画出的节点不重建;
//   - 定稿:最后一次对齐(带脚本与 on* 属性),按文档顺序逐个换成新建的 script 执行
//     (外链脚本等加载完再执行下一个,失败就跳过并提示),最后补发 DOMContentLoaded 与 load;
//   - 尺寸上报、追问(ace.send / sendPrompt)与链接拦截、主题更新、存储顶替。
export function inlineVisualRuntime(config, host) {
  var options = config || {};
  var win = host && host.window ? host.window : window;
  var doc = host && host.document ? host.document : win.document;
  var PROTOCOL = options.protocol || 1;
  var MAX_CHARS = options.sendMaxChars || 4000;
  var labels = options.labels || {};
  var XHTML = 'http://www.w3.org/1999/xhtml';
  var BLOCKED_TAGS = {
    meta: 1, base: 1, iframe: 1, frame: 1, frameset: 1, object: 1, embed: 1, portal: 1, applet: 1,
  };
  var state = {
    finalized: false,
    lastCode: null,
    lastHeight: -1,
    lastTrusted: -Infinity,
    themeListeners: [],
  };

  function now() {
    if (host && typeof host.now === 'function') return host.now();
    var perf = win.performance;
    return perf && typeof perf.now === 'function' ? perf.now() : Date.now();
  }

  function post(message) {
    message.ace_visual = PROTOCOL;
    if (host && typeof host.post === 'function') {
      host.post(message);
      return;
    }
    try {
      win.parent.postMessage(message, '*');
    } catch (error) {
      // 外层已卸载:没人接收,忽略。
    }
  }

  function rootElement() {
    if (host && host.root) return host.root;
    return doc.getElementById('ace-root');
  }

  function parseFragment(code) {
    if (host && typeof host.parse === 'function') return host.parse(code);
    var template = doc.createElement('template');
    template.innerHTML = code;
    return template.content;
  }

  // 流式期间不执行任何脚本:整段剥掉已闭合的 script 元素,
  // 末尾还没闭合(或只写了开标签)的 script 一直剥到正文末尾。
  function stripScripts(code) {
    var text = String(code || '').replace(/<script\b[\s\S]*?<\/script\s*>/gi, '');
    var open = text.search(/<script\b/i);
    return open >= 0 ? text.slice(0, open) : text;
  }

  function lowerName(node) {
    return String(node.localName || node.nodeName || '').toLowerCase();
  }

  // 去掉能让框离开自身文档的元素:meta(refresh)、base、嵌套框与插件;
  // link 只留样式表(内容安全策略另外限制它的来源)。
  function sanitize(parent) {
    var child = parent.firstChild;
    while (child) {
      var next = child.nextSibling;
      if (child.nodeType === 1) {
        var name = lowerName(child);
        var rel = name === 'link' ? String(child.getAttribute('rel') || '').toLowerCase() : '';
        if (BLOCKED_TAGS[name] || (name === 'link' && rel !== 'stylesheet')) {
          parent.removeChild(child);
        } else {
          sanitize(child);
        }
      }
      child = next;
    }
  }

  function attributeAllowed(name, final) {
    return final || !/^on/i.test(name);
  }

  function sameKind(current, wanted) {
    if (current.nodeType !== wanted.nodeType) return false;
    if (current.nodeType !== 1) return true;
    return (current.namespaceURI || XHTML) === (wanted.namespaceURI || XHTML)
      && lowerName(current) === lowerName(wanted);
  }

  function syncAttributes(current, wanted, final) {
    var keep = {};
    var index;
    var attr;
    for (index = 0; index < wanted.attributes.length; index += 1) {
      attr = wanted.attributes[index];
      if (!attributeAllowed(attr.name, final)) continue;
      keep[attr.name] = true;
      if (current.getAttribute(attr.name) !== attr.value) {
        if (attr.namespaceURI) current.setAttributeNS(attr.namespaceURI, attr.name, attr.value);
        else current.setAttribute(attr.name, attr.value);
      }
    }
    for (index = current.attributes.length - 1; index >= 0; index -= 1) {
      attr = current.attributes[index];
      if (!keep[attr.name]) current.removeAttribute(attr.name);
    }
  }

  function stripEventAttributes(element) {
    for (var index = element.attributes.length - 1; index >= 0; index -= 1) {
      var name = element.attributes[index].name;
      if (/^on/i.test(name)) element.removeAttribute(name);
    }
  }

  function importTree(node, final) {
    var copy = doc.importNode(node, false);
    if (copy.nodeType === 1) {
      if (!final) stripEventAttributes(copy);
      var child = node.firstChild;
      while (child) {
        copy.appendChild(importTree(child, final));
        child = child.nextSibling;
      }
    }
    return copy;
  }

  // 按位置对齐子节点:同类节点更新属性 / 文本后递归,不同就替换,多出的删掉。
  // 流式内容几乎只在末尾追加,这样已画出的形状保持原节点,不会闪烁。
  function morphChildren(current, wanted, final) {
    var wantedChildren = [];
    var child = wanted.firstChild;
    while (child) {
      wantedChildren.push(child);
      child = child.nextSibling;
    }
    for (var index = 0; index < wantedChildren.length; index += 1) {
      var source = wantedChildren[index];
      var target = current.childNodes[index];
      if (!target) {
        current.appendChild(importTree(source, final));
      } else if (!sameKind(target, source)) {
        current.replaceChild(importTree(source, final), target);
      } else if (source.nodeType === 1) {
        syncAttributes(target, source, final);
        morphChildren(target, source, final);
      } else if (target.nodeValue !== source.nodeValue) {
        target.nodeValue = source.nodeValue;
      }
    }
    while (current.childNodes.length > wantedChildren.length) {
      current.removeChild(current.lastChild);
    }
  }

  function measure() {
    var root = rootElement();
    if (!root || typeof root.getBoundingClientRect !== 'function') return;
    var height = Math.ceil(root.getBoundingClientRect().height);
    if (height === state.lastHeight) return;
    state.lastHeight = height;
    post({ type: 'size', height: height });
  }

  function notice(text) {
    var root = rootElement();
    if (!text || !root || state.noticeShown) return;
    state.noticeShown = true;
    var line = doc.createElement('p');
    line.setAttribute('class', 'ace-visual-notice');
    line.textContent = text;
    root.appendChild(line);
  }

  function report(message) {
    post({ type: 'error', message: String(message || 'error').slice(0, 500) });
  }

  function dispatch(target, type) {
    try {
      var EventType = win.Event;
      var event = typeof EventType === 'function'
        ? new EventType(type, { bubbles: type === 'DOMContentLoaded' })
        : null;
      if (event && target && typeof target.dispatchEvent === 'function') target.dispatchEvent(event);
    } catch (error) {
      report(error && error.message);
    }
  }

  function collectScripts(parent, out) {
    var child = parent.firstChild;
    while (child) {
      if (child.nodeType === 1) {
        if (lowerName(child) === 'script') out.push(child);
        else collectScripts(child, out);
      }
      child = child.nextSibling;
    }
    return out;
  }

  // 定稿后按文档顺序执行脚本:把每个不会执行的 script 换成新建的 script。外链脚本等
  // load / error(最多 15 秒)再继续;全部完成后补发 DOMContentLoaded 与 load —— 模型写的
  // 页面常把初始化挂在这两个事件上,而隔离框加载时它们早已触发过。
  function runScripts(done) {
    var root = rootElement();
    var scripts = root ? collectScripts(root, []) : [];
    var position = 0;
    function next() {
      if (position >= scripts.length) {
        done();
        return;
      }
      var old = scripts[position];
      position += 1;
      if (!old.parentNode) {
        next();
        return;
      }
      var fresh = doc.createElement('script');
      for (var index = 0; index < old.attributes.length; index += 1) {
        fresh.setAttribute(old.attributes[index].name, old.attributes[index].value);
      }
      var source = fresh.getAttribute('src');
      if (!source) {
        fresh.text = old.textContent;
        old.parentNode.replaceChild(fresh, old);
        next();
        return;
      }
      var settled = false;
      var timer = null;
      var finish = function (failed) {
        if (settled) return;
        settled = true;
        if (timer) win.clearTimeout(timer);
        if (failed) {
          notice(labels.scriptFailed);
          report('script failed to load: ' + source);
        }
        next();
      };
      fresh.addEventListener('load', function () { finish(false); });
      fresh.addEventListener('error', function () { finish(true); });
      timer = win.setTimeout(function () { finish(true); }, 15000);
      old.parentNode.replaceChild(fresh, old);
    }
    next();
  }

  function render(code, final) {
    if (state.finalized) return;
    var text = String(code || '');
    if (!final && text === state.lastCode) return;
    state.lastCode = text;
    var root = rootElement();
    if (!root) return;
    var fragment = parseFragment(final ? text : stripScripts(text));
    sanitize(fragment);
    morphChildren(root, fragment, !!final);
    measure();
    if (!final) return;
    state.finalized = true;
    runScripts(function () {
      dispatch(doc, 'DOMContentLoaded');
      dispatch(win, 'load');
      measure();
    });
  }

  function isColor(value) {
    return typeof value === 'string' && /^#[0-9a-f]{3,8}$/i.test(value);
  }

  function themeCss(vars) {
    var body = '';
    for (var name in vars) {
      if (!Object.prototype.hasOwnProperty.call(vars, name) || !/^--[a-z0-9-]+$/.test(name)) continue;
      var value = String(vars[name] || '');
      if (!value || value.length > 300 || /[{}<>;\\]/.test(value)) continue;
      body += name + ':' + value + ';';
    }
    return ':root{' + body + '}';
  }

  // 主题切换:只换变量与 data-mode,不动内容、不重跑脚本;canvas 图表经 onThemeChange 自己重设颜色。
  function applyTheme(message) {
    var mode = message.mode === 'dark' ? 'dark' : 'light';
    if (doc.documentElement) doc.documentElement.setAttribute('data-mode', mode);
    var style = typeof doc.getElementById === 'function' ? doc.getElementById('ace-theme') : null;
    if (style && message.vars && typeof message.vars === 'object') style.textContent = themeCss(message.vars);
    api.mode = mode;
    if (Array.isArray(message.palette)) {
      var palette = message.palette.filter(isColor).slice(0, 8);
      if (palette.length) api.palette = palette;
    }
    var listeners = state.themeListeners.slice();
    for (var index = 0; index < listeners.length; index += 1) {
      try {
        listeners[index]({ mode: mode, palette: api.palette.slice() });
      } catch (error) {
        report(error && error.message);
      }
    }
  }

  // 用户刚操作过才算数:优先看浏览器的用户激活状态,没有这个接口时看 1 秒内是否有
  // 可信的点击 / 按键 / 输入事件。外层还会再按自己的激活状态与限频把一道关。
  function userActive() {
    var activation = win.navigator && win.navigator.userActivation;
    if (activation && typeof activation.isActive === 'boolean') return activation.isActive;
    return now() - state.lastTrusted < 1000;
  }

  var api = {
    mode: options.mode === 'dark' ? 'dark' : 'light',
    palette: Array.isArray(options.palette) ? options.palette.filter(isColor).slice(0, 8) : [],
    send: function (text) {
      if (typeof text !== 'string') return false;
      var value = text.trim();
      if (!value || value.length > MAX_CHARS || !userActive()) return false;
      post({ type: 'send', text: value });
      return true;
    },
    openLink: function (url) {
      if (typeof url !== 'string' || !/^(?:https?:|mailto:)/i.test(url.trim()) || !userActive()) return false;
      post({ type: 'link', url: url.trim() });
      return true;
    },
    onThemeChange: function (listener) {
      if (typeof listener === 'function') state.themeListeners.push(listener);
    },
  };

  function handleMessage(data) {
    if (!data || typeof data !== 'object' || data.ace_visual !== PROTOCOL) return;
    if (data.type === 'render') render(data.code, data.final === true);
    else if (data.type === 'theme') applyTheme(data);
  }

  function memoryStorage() {
    var data = {};
    return {
      getItem: function (key) {
        var name = String(key);
        return Object.prototype.hasOwnProperty.call(data, name) ? data[name] : null;
      },
      setItem: function (key, value) { data[String(key)] = String(value); },
      removeItem: function (key) { delete data[String(key)]; },
      clear: function () { data = {}; },
      key: function (index) {
        var keys = Object.keys(data);
        return index >= 0 && index < keys.length ? keys[index] : null;
      },
      get length() { return Object.keys(data).length; },
    };
  }

  function installStorage() {
    var names = ['localStorage', 'sessionStorage'];
    for (var index = 0; index < names.length; index += 1) {
      var usable = false;
      try {
        usable = !!win[names[index]];
      } catch (error) {
        usable = false;
      }
      if (usable) continue;
      try {
        Object.defineProperty(win, names[index], { configurable: true, value: memoryStorage() });
      } catch (error) {
        // 顶替失败就算了:脚本访问存储时自己会报错。
      }
    }
  }

  function anchorFrom(target) {
    var node = target && target.nodeType === 3 ? target.parentNode : target;
    return node && typeof node.closest === 'function' ? node.closest('a') : null;
  }

  function boot() {
    installStorage();
    win.ace = api;
    win.sendPrompt = api.send;
    try {
      win.open = function (url) {
        api.openLink(String(url || ''));
        return null;
      };
    } catch (error) {
      // 某些宿主不允许改 window.open。
    }
    var trusted = function (event) {
      if (event && event.isTrusted) state.lastTrusted = now();
    };
    var activationEvents = ['pointerdown', 'mousedown', 'click', 'keydown', 'input', 'change', 'touchend'];
    for (var index = 0; index < activationEvents.length; index += 1) {
      doc.addEventListener(activationEvents[index], trusted, true);
    }
    // 框内链接一律交给外层用系统浏览器打开,框本身绝不导航。
    doc.addEventListener('click', function (event) {
      var anchor = anchorFrom(event.target);
      if (!anchor) return;
      event.preventDefault();
      var href = anchor.getAttribute('href') || anchor.getAttribute('xlink:href') || '';
      if (href && href.charAt(0) !== '#') api.openLink(href);
    }, true);
    doc.addEventListener('submit', function (event) { event.preventDefault(); }, true);
    win.addEventListener('message', function (event) {
      if (event.source !== win.parent) return;
      handleMessage(event.data);
    });
    win.addEventListener('error', function (event) {
      if (event && event.message) report(event.message);
    });
    win.addEventListener('unhandledrejection', function (event) {
      var reason = event && event.reason;
      report(reason && reason.message ? reason.message : String(reason));
    });
    var root = rootElement();
    if (root && typeof win.ResizeObserver === 'function') {
      new win.ResizeObserver(function () { measure(); }).observe(root);
    }
    post({ type: 'ready' });
  }

  if (!host || host.boot !== false) boot();

  return {
    api: api,
    state: state,
    render: render,
    stripScripts: stripScripts,
    sanitize: sanitize,
    morphChildren: morphChildren,
    applyTheme: applyTheme,
    handleMessage: handleMessage,
    themeCss: themeCss,
    userActive: userActive,
    memoryStorage: memoryStorage,
  };
}
