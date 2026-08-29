import {
  OFFICE_PREVIEW_ZOOM_DEFAULT,
  OFFICE_PREVIEW_ZOOM_MAX,
  OFFICE_PREVIEW_ZOOM_MIN,
} from './officePreviewZoom.js';

export const PRESENTATION_PREVIEW_SOURCE = 'acecode:presentation-preview';

// pptx2html@0.3.4 treats optional OOXML parts as mandatory and mishandles
// absolute package relationship targets. Keep these exact patches coupled to
// the pinned minified build so an upgrade cannot alter unverified code shapes.
const TABLE_STYLES_LOAD = 'e.next=20,L(n,"ppt/tableStyles.xml")';
const OPTIONAL_TABLE_STYLES_LOAD = 'e.next=20,n.file("ppt/tableStyles.xml")?L(n,"ppt/tableStyles.xml"):{"a:tblStyleLst":{"a:tblStyle":[]}}';
const XML_PART_LOAD = 'e.t0=l,e.next=3,t.file(n).async("text")';
const GUARDED_XML_PART_LOAD = 'e.t0=l,e.next=3,function(){var e=t.file(n);if(!e)throw new Error("Missing PPTX part: "+n);return e.async("text")}()';
const RELATIONSHIP_TARGET_LOAD = '.replace("../","ppt/")';
const NORMALIZED_RELATIONSHIP_TARGET_LOAD = '.replace(/^\\/+/,"").replace("../","ppt/")';
const THEME_TARGET_LOAD = 'L(t,"ppt/"+i)';
const NORMALIZED_THEME_TARGET_LOAD = 'L(t,i.charAt(0)==="/"?i.replace(/^\\/+/, ""):"ppt/"+i)';
const LAYOUT_COLOR_OVERRIDE_LOAD = 'h["p:sldLayout"]["p:clrMapOvr"]["a:overrideClrMapping"]';
const OPTIONAL_LAYOUT_COLOR_OVERRIDE_LOAD = '(h["p:sldLayout"]["p:clrMapOvr"]||{})["a:overrideClrMapping"]';

function scriptString(value) {
  return JSON.stringify(String(value));
}

export function escapePresentationRendererSource(source) {
  return String(source || '').replace(/<\/script/gi, '<\\/script');
}

export function presentationRendererSource(source) {
  const rendererSource = String(source || '');
  const firstMatch = rendererSource.indexOf(TABLE_STYLES_LOAD);
  const firstXmlLoad = rendererSource.indexOf(XML_PART_LOAD);
  const relationshipTargetLoads = rendererSource.split(RELATIONSHIP_TARGET_LOAD).length - 1;
  const themeTargetLoads = rendererSource.split(THEME_TARGET_LOAD).length - 1;
  const layoutColorOverrideLoads = rendererSource.split(LAYOUT_COLOR_OVERRIDE_LOAD).length - 1;
  if (firstMatch < 0
      || rendererSource.indexOf(TABLE_STYLES_LOAD, firstMatch + 1) >= 0
      || firstXmlLoad < 0
      || rendererSource.indexOf(XML_PART_LOAD, firstXmlLoad + 1) >= 0
      || relationshipTargetLoads !== 9
      || themeTargetLoads !== 1
      || layoutColorOverrideLoads !== 1) {
    throw new Error('Unsupported pptx2html renderer build');
  }
  return rendererSource
    .replace(TABLE_STYLES_LOAD, OPTIONAL_TABLE_STYLES_LOAD)
    .replace(XML_PART_LOAD, GUARDED_XML_PART_LOAD)
    .replace(THEME_TARGET_LOAD, NORMALIZED_THEME_TARGET_LOAD)
    .replace(LAYOUT_COLOR_OVERRIDE_LOAD, OPTIONAL_LAYOUT_COLOR_OVERRIDE_LOAD)
    .split(RELATIONSHIP_TARGET_LOAD)
    .join(NORMALIZED_RELATIONSHIP_TARGET_LOAD);
}

export function createPresentationPreviewChannel(cryptoApi = globalThis.crypto) {
  if (typeof cryptoApi?.randomUUID === 'function') return cryptoApi.randomUUID();
  if (typeof cryptoApi?.getRandomValues === 'function') {
    const bytes = new Uint8Array(16);
    cryptoApi.getRandomValues(bytes);
    return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
  }
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

export function presentationFrameDocument(rendererSource, channel) {
  const source = scriptString(PRESENTATION_PREVIEW_SOURCE);
  const frameChannel = scriptString(channel);
  const renderer = escapePresentationRendererSource(rendererSource);
  return `<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; background: #e5e7eb; }
    body { color: #111827; font-family: Arial, sans-serif; }
    #ace-presentation-viewport { position: relative; width: 100%; height: 100%; overflow: auto; }
    #ace-presentation-stage { position: relative; width: 100%; height: 100%; }
    #ace-presentation-root { position: absolute; top: 0; left: 0; width: 100%; min-height: 100%; transform-origin: top left; }
    #ace-presentation-root > section { margin: 0 !important; }
    #ace-presentation-root > section[hidden] { display: none !important; }
  </style>
</head>
<body>
  <div id="ace-presentation-viewport">
    <div id="ace-presentation-stage">
      <div id="ace-presentation-root"></div>
    </div>
  </div>
  <script>${renderer}</script>
  <script>
    (() => {
      const source = ${source};
      const channel = ${frameChannel};
      const viewport = document.getElementById('ace-presentation-viewport');
      const stage = document.getElementById('ace-presentation-stage');
      const root = document.getElementById('ace-presentation-root');
      const minimumZoom = ${OFFICE_PREVIEW_ZOOM_MIN};
      const maximumZoom = ${OFFICE_PREVIEW_ZOOM_MAX};
      let zoom = ${OFFICE_PREVIEW_ZOOM_DEFAULT};
      let slides = [];
      let slideIndex = 0;
      let rendering = false;
      let rendered = false;
      let resizeBound = false;
      const send = (status, detail = '', payload = {}) => parent.postMessage({ source, channel, status, detail, ...payload }, '*');
      const clampZoom = (value) => {
        const numeric = Number(value);
        if (!Number.isFinite(numeric)) return zoom;
        return Math.round(Math.min(maximumZoom, Math.max(minimumZoom, numeric)) * 100) / 100;
      };
      const statePayload = () => ({
        slideIndex,
        slideCount: slides.length,
        zoom,
      });
      const applyState = () => {
        if (!slides.length) return;
        slideIndex = Math.min(slides.length - 1, Math.max(0, slideIndex));
        slides.forEach((slide, index) => {
          slide.hidden = index !== slideIndex;
        });
        const slide = slides[slideIndex];
        const slideWidth = Math.max(1, slide.offsetWidth || Number.parseFloat(slide.style.width) || 1);
        const slideHeight = Math.max(1, slide.offsetHeight || Number.parseFloat(slide.style.height) || 1);
        const availableWidth = Math.max(1, viewport.clientWidth - 32);
        const availableHeight = Math.max(1, viewport.clientHeight - 32);
        const fitScale = Math.min(availableWidth / slideWidth, availableHeight / slideHeight);
        const scale = fitScale * zoom;
        const scaledWidth = Math.max(1, Math.round(slideWidth * scale));
        const scaledHeight = Math.max(1, Math.round(slideHeight * scale));
        stage.style.width = scaledWidth + 'px';
        stage.style.height = scaledHeight + 'px';
        stage.style.marginLeft = Math.max(16, Math.round((viewport.clientWidth - scaledWidth) / 2)) + 'px';
        stage.style.marginTop = Math.max(16, Math.round((viewport.clientHeight - scaledHeight) / 2)) + 'px';
        root.style.width = slideWidth + 'px';
        root.style.height = slideHeight + 'px';
        root.style.minHeight = '0';
        root.style.transform = 'scale(' + scale + ')';
      };
      const reportState = () => send('state', '', statePayload());
      const setZoom = (value) => {
        const nextZoom = clampZoom(value);
        if (nextZoom === zoom) return;
        zoom = nextZoom;
        applyState();
        reportState();
      };
      const navigate = (delta) => {
        if (!slides.length) return;
        const nextIndex = Math.min(
          slides.length - 1,
          Math.max(0, slideIndex + Math.sign(Number(delta) || 0)),
        );
        if (nextIndex === slideIndex) return;
        slideIndex = nextIndex;
        viewport.scrollLeft = 0;
        viewport.scrollTop = 0;
        applyState();
        reportState();
      };
      const bindResize = () => {
        if (resizeBound) return;
        resizeBound = true;
        window.addEventListener('resize', applyState);
        if (typeof ResizeObserver === 'function') {
          const observer = new ResizeObserver(applyState);
          observer.observe(viewport);
        }
      };
      viewport.addEventListener('wheel', (event) => {
        if (!event.ctrlKey) return;
        event.preventDefault();
        if (event.deltaY === 0) return;
        setZoom(zoom + (event.deltaY < 0 ? 0.1 : -0.1));
      }, { passive: false });
      window.addEventListener('message', async (event) => {
        const data = event.data || {};
        if (event.source !== parent || data.source !== source || data.channel !== channel) return;
        if (data.status === 'ping') {
          send('ready');
          return;
        }
        if (data.status === 'zoom') {
          setZoom(data.zoom);
          return;
        }
        if (data.status === 'navigate') {
          navigate(data.delta);
          return;
        }
        if (data.status !== 'render' || !(data.buffer instanceof ArrayBuffer) || rendering || rendered) return;
        rendering = true;
        try {
          root.replaceChildren();
          if (typeof window.pptx2html !== 'function') throw new Error('pptx2html renderer unavailable');
          await Promise.resolve(window.pptx2html(data.buffer, root));
          slides = Array.from(root.querySelectorAll('section'));
          if (!slides.length) throw new Error('Presentation contains no slides');
          slideIndex = 0;
          rendered = true;
          bindResize();
          applyState();
          send('complete', String(slides.length), statePayload());
        } catch (error) {
          send('error', error && error.message ? error.message : String(error));
        } finally {
          rendering = false;
        }
      });
      send('ready');
    })();
  <\/script>
</body>
</html>`;
}

export function presentationPreviewMessage(event, frameWindow, channel) {
  const data = event?.data;
  if (event?.source !== frameWindow
      || !data
      || data.source !== PRESENTATION_PREVIEW_SOURCE
      || data.channel !== channel
      || !['ready', 'complete', 'error', 'state'].includes(data.status)) {
    return null;
  }
  const message = {
    status: data.status,
    detail: typeof data.detail === 'string' ? data.detail : '',
  };
  if (Number.isInteger(data.slideIndex) && data.slideIndex >= 0) {
    message.slideIndex = data.slideIndex;
  }
  if (Number.isInteger(data.slideCount) && data.slideCount >= 0) {
    message.slideCount = data.slideCount;
  }
  if (Number.isFinite(data.zoom)
      && data.zoom >= OFFICE_PREVIEW_ZOOM_MIN
      && data.zoom <= OFFICE_PREVIEW_ZOOM_MAX) {
    message.zoom = data.zoom;
  }
  return message;
}

export function postPresentationPreviewMessage(frameWindow, channel, status, payload = {}, transfer = []) {
  if (!frameWindow || typeof frameWindow.postMessage !== 'function') return false;
  frameWindow.postMessage({
    source: PRESENTATION_PREVIEW_SOURCE,
    channel,
    status,
    ...payload,
  }, '*', transfer);
  return true;
}
