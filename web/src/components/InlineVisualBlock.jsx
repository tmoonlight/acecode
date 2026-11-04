// 对话内可视化:把助手回复里顶层的 ```visual 代码块画在隔离框里(openspec add-inline-visuals,
// docs/inline-visuals.md)。
//
// - 隔离:iframe 只有 sandbox="allow-scripts"(不透明源,读不到本页面、存储与 Cookie),
//   srcdoc 第一行是内容安全策略,只放行四个公共 CDN,本机 daemon 请求不到。
// - 边写边画:同一个组件实例从流式用到定稿(Message.jsx 按块序号做 key),srcdoc 只生成一次,
//   内容与主题更新都经 postMessage;iframe 就绪前的消息先排队,只留最新一份。
// - 只认自己 iframe 发来的消息(event.source),追问与链接再按本页面的用户激活状态、限频、
//   长度与协议白名单把关。
// - 定稿后内容若又变化(重新生成等),换一个新 iframe,脚本按新内容重新执行。
// - 框内脚本引用内置图表库(Chart.js 4 / d3 7)时,由这里把本地副本递进去,断网也能画。
import { memo, useContext, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import {
  INLINE_VISUAL_PROTOCOL,
  buildInlineVisualSrcdoc,
  clampInlineVisualHeight,
  createInlineVisualSendLimiter,
  inlineVisualCodeKey,
  inlineVisualLinkUrl,
  inlineVisualMessage,
  inlineVisualSendText,
  readInlineVisualTheme,
} from '../lib/inlineVisualFrame.js';
import { isInlineVisualLibrary, loadInlineVisualLibrary } from '../lib/inlineVisualLibraries.js';
import { copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { openExternalUrl } from '../lib/externalUrl.js';
import { InlineVisualBridgeContext } from './InlineVisualBridgeContext.jsx';
import { toast } from './Toast.jsx';

// 隔离框 sandbox 恰好只有 allow-scripts:加 allow-same-origin 就等于让框内代码拿到本页面权限。
export const INLINE_VISUAL_SANDBOX = 'allow-scripts';

// 同一段代码重新挂载(滚回历史、切回会话)时先用上次的高度,不跳版。
const HEIGHT_MEMORY_LIMIT = 200;
const heightMemory = new Map();

function rememberHeight(key, height) {
  if (!key || !height) return;
  heightMemory.delete(key);
  heightMemory.set(key, height);
  while (heightMemory.size > HEIGHT_MEMORY_LIMIT) {
    heightMemory.delete(heightMemory.keys().next().value);
  }
}

// 框内的点击会让本页面也获得用户激活(激活沿父级向上传递),框内脚本伪造不了。
// 没有 userActivation 接口的旧 WebKit 退而要求焦点在这个 iframe 上(框内脚本不能自己抢焦点)。
function parentUserActive(iframe) {
  const activation = globalThis.navigator?.userActivation;
  if (activation && typeof activation.isActive === 'boolean') return activation.isActive;
  return !!iframe && globalThis.document?.activeElement === iframe;
}

function InlineVisualFrame({ code, final, onEscaped }) {
  const bridge = useContext(InlineVisualBridgeContext);
  const codeKey = final ? inlineVisualCodeKey(code) : '';
  const [height, setHeight] = useState(() => (codeKey && heightMemory.get(codeKey)) || null);
  const iframeRef = useRef(null);
  const readyRef = useRef(false);
  const latestRenderRef = useRef(null);
  const loadsRef = useRef(0);
  const escapeTimerRef = useRef(0);
  const themeSignatureRef = useRef('');
  const codeKeyRef = useRef(codeKey);
  const bridgeRef = useRef(bridge);
  const limiterRef = useRef(null);
  if (!limiterRef.current) limiterRef.current = createInlineVisualSendLimiter();
  codeKeyRef.current = codeKey;
  bridgeRef.current = bridge;

  const scriptFailedLabel = '图表库加载失败,可能是网络连不上 CDN';
  const srcdoc = useMemo(() => {
    const theme = readInlineVisualTheme();
    themeSignatureRef.current = JSON.stringify(theme);
    return buildInlineVisualSrcdoc({ theme, labels: { scriptFailed: scriptFailedLabel } });
    // 外壳每个 iframe 只生成一次;之后的主题变化走 postMessage,不重新加载。
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 框内运行时报到(ready)之前不发消息;报到时补发当前主题与最新一次内容。
  // 只保留最新一份内容,流式期间不会在 iframe 就绪前堆积几十条。
  const postRef = useRef(null);
  postRef.current = (message) => {
    const payload = { ace_visual: INLINE_VISUAL_PROTOCOL, ...message };
    if (message.type === 'render') latestRenderRef.current = payload;
    const target = iframeRef.current?.contentWindow;
    if (readyRef.current && target) target.postMessage(payload, '*');
  };

  useEffect(() => {
    postRef.current({ type: 'render', code, final });
  }, [code, final]);

  // 握手:标记就绪,补发当前主题与最新一次内容(重载后的新运行时同样走这里)。
  const handshakeRef = useRef(null);
  handshakeRef.current = () => {
    const target = iframeRef.current?.contentWindow;
    if (!target) return;
    readyRef.current = true;
    if (escapeTimerRef.current) {
      clearTimeout(escapeTimerRef.current);
      escapeTimerRef.current = 0;
    }
    const theme = readInlineVisualTheme();
    themeSignatureRef.current = JSON.stringify(theme);
    target.postMessage({ ace_visual: INLINE_VISUAL_PROTOCOL, type: 'theme', ...theme }, '*');
    if (latestRenderRef.current) target.postMessage(latestRenderRef.current, '*');
  };

  // 监听必须在提交阶段同步挂上:框内运行时启动即发 ready,普通 effect 可能晚于它。
  useLayoutEffect(() => {
    const onMessage = (event) => {
      const frame = iframeRef.current;
      if (!frame || event.source !== frame.contentWindow) return;
      const message = inlineVisualMessage(event.data);
      if (!message) return;
      if (message.type === 'ready') {
        handshakeRef.current();
      } else if (message.type === 'size') {
        const next = clampInlineVisualHeight(message.height);
        if (!next) return;
        setHeight(next);
        rememberHeight(codeKeyRef.current, next);
      } else if (message.type === 'send') {
        const text = inlineVisualSendText(message.text);
        const send = bridgeRef.current?.send;
        if (!text || typeof send !== 'function' || !parentUserActive(frame)) return;
        if (!limiterRef.current()) return;
        send(text);
      } else if (message.type === 'link') {
        const url = inlineVisualLinkUrl(message.url);
        if (!url || !parentUserActive(frame)) return;
        const open = bridgeRef.current?.openLink || openExternalUrl;
        Promise.resolve(open(url)).catch(() => {});
      } else if (message.type === 'library') {
        // 框内脚本引用了内置图表库:把本地副本递进去(框自己请求不到本机),给不了就回空,
        // 框内运行时会照原地址去 CDN 加载。
        const request = message.request;
        if (!Number.isSafeInteger(request)) return;
        const reply = (code) => {
          try {
            event.source.postMessage({ ace_visual: INLINE_VISUAL_PROTOCOL, type: 'library', request, code }, '*');
          } catch {
            // 框已卸载。
          }
        };
        if (!isInlineVisualLibrary(message.name)) {
          reply(null);
          return;
        }
        loadInlineVisualLibrary(message.name).then(reply, () => reply(null));
      } else if (message.type === 'error') {
        console.debug('[inline-visual]', String(message.message || '').slice(0, 300));
      }
    };
    window.addEventListener('message', onMessage);
    return () => {
      window.removeEventListener('message', onMessage);
      if (escapeTimerRef.current) clearTimeout(escapeTimerRef.current);
    };
  }, []);

  // 主题切换(浅色 / 深色、配色主题):只推新变量,框内不重建内容、不重跑脚本。
  useEffect(() => {
    const root = globalThis.document?.documentElement;
    if (!root || typeof MutationObserver !== 'function') return undefined;
    let frameId = 0;
    const update = () => {
      frameId = 0;
      const theme = readInlineVisualTheme();
      const signature = JSON.stringify(theme);
      if (signature === themeSignatureRef.current) return;
      themeSignatureRef.current = signature;
      postRef.current({ type: 'theme', ...theme });
    };
    const observer = new MutationObserver(() => {
      if (!frameId) frameId = requestAnimationFrame(update);
    });
    observer.observe(root, { attributes: true });
    return () => {
      observer.disconnect();
      if (frameId) cancelAnimationFrame(frameId);
    };
  }, []);

  // srcdoc 从不改变,再次 load 要么是 iframe 被移动后重载(运行时会重新报到,内容随即恢复),
  // 要么是框内脚本把文档导航走了 —— 2 秒内没有报到就按后者处理,改为显示源码。
  const handleLoad = () => {
    loadsRef.current += 1;
    if (loadsRef.current === 1) {
      // 兜底:ready 若在监听挂上之前就发出了,首次 load 时运行时一定已经在听消息。
      if (!readyRef.current) handshakeRef.current();
      return;
    }
    readyRef.current = false;
    if (escapeTimerRef.current) clearTimeout(escapeTimerRef.current);
    escapeTimerRef.current = setTimeout(() => {
      escapeTimerRef.current = 0;
      if (!readyRef.current) onEscaped();
    }, 2000);
  };

  return (
    <iframe
      ref={iframeRef}
      className="ace-inline-visual-frame"
      title="可视化图表"
      sandbox={INLINE_VISUAL_SANDBOX}
      srcDoc={srcdoc}
      referrerPolicy="no-referrer"
      loading="lazy"
      scrolling="no"
      onLoad={handleLoad}
      style={height ? { height } : undefined}
    />
  );
}

export const InlineVisualBlock = memo(function InlineVisualBlock({ code, closed, streaming }) {
  const text = String(code || '');
  const final = !!closed || !streaming;
  const [generation, setGeneration] = useState(0);
  const [escaped, setEscaped] = useState(false);
  const finalCodeRef = useRef(null);

  // 定稿后内容变化(重新生成、编辑后重放):换新 iframe 按新内容重画并重新执行脚本。
  useEffect(() => {
    if (finalCodeRef.current !== null && finalCodeRef.current !== text) {
      finalCodeRef.current = null;
      setGeneration((value) => value + 1);
      setEscaped(false);
    }
    if (final) finalCodeRef.current = text;
  }, [text, final]);

  const copy = async (event) => {
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (error) {
      toast({ kind: 'err', text: '复制失败:' + (error?.message || '') });
    }
  };

  return (
    <div className="ace-md-block">
      <div className="ace-inline-visual" data-inline-visual="true" data-inline-visual-final={final ? 'true' : undefined}>
        {escaped ? (
          <pre className="ace-inline-visual-source"><code>{text}</code></pre>
        ) : (
          <InlineVisualFrame
            key={generation}
            code={text}
            final={final}
            onEscaped={() => setEscaped(true)}
          />
        )}
        <button
          type="button"
          className="ace-code-copy-btn ace-inline-visual-copy"
          title="复制代码"
          aria-label="复制代码"
          onClick={copy}
        >
          <span
            className="ace-icon"
            style={{ width: 14, height: 14, '--ace-icon-url': "url('/vs-icons/Copy.svg')" }}
            data-monochrome="true"
            aria-hidden="true"
          />
        </button>
      </div>
    </div>
  );
});
