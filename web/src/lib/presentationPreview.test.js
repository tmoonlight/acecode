import assert from 'node:assert/strict';
import fs from 'node:fs';
import {
  PRESENTATION_PREVIEW_SOURCE,
  escapePresentationRendererSource,
  postPresentationPreviewMessage,
  presentationFrameDocument,
  presentationPreviewMessage,
  presentationRendererSource,
} from './presentationPreview.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('presentation renderer source cannot terminate its containing script', () => {
  assert.equal(
    escapePresentationRendererSource('before</SCRIPT>after'),
    'before<\\/script>after',
  );
});

run('presentation renderer tolerates generated PPTX optional parts and absolute targets', () => {
  const relationshipLoads = Array.from(
    { length: 9 },
    (_, index) => `target${index}.replace("../","ppt/")`,
  ).join(';');
  const source = `before;e.next=20,L(n,"ppt/tableStyles.xml");e.t0=l,e.next=3,t.file(n).async("text");${relationshipLoads};L(t,"ppt/"+i);h["p:sldLayout"]["p:clrMapOvr"]["a:overrideClrMapping"];after`;
  const patched = presentationRendererSource(source);
  assert.match(patched, /n\.file\("ppt\/tableStyles\.xml"\)\?L\(n,"ppt\/tableStyles\.xml"\)/);
  assert.match(patched, /"a:tblStyle":\[\]/);
  assert.match(patched, /Missing PPTX part:/);
  assert.equal((patched.match(/replace\(\/\^\\\/\+\//g) || []).length, 10);
  assert.match(patched, /i\.charAt\(0\)==="\/"/);
  assert.match(patched, /\(h\["p:sldLayout"\]\["p:clrMapOvr"\]\|\|\{\}\)\["a:overrideClrMapping"\]/);
  assert.doesNotMatch(patched, /;e\.next=20,L\(n,"ppt\/tableStyles\.xml"\);/);
  assert.throws(() => presentationRendererSource('unrecognized build'), /Unsupported pptx2html renderer build/);
});

run('presentation frame embeds the renderer and a channel-bound bridge', () => {
  const html = presentationFrameDocument('window.pptx2html = function () {};', 'channel-1');
  assert.match(html, /window\.pptx2html/);
  assert.match(html, /channel-1/);
  assert.match(html, /event\.source !== parent/);
  assert.match(html, /data\.buffer instanceof ArrayBuffer/);
  assert.match(html, /typeof pptx2html === 'function'/);
  assert.match(html, /Promise\.resolve\(renderPresentation\(data\.buffer, root\)\)/);
  assert.match(html, /Array\.from\(root\.querySelectorAll\('section'\)\)/);
  assert.match(html, /slide\.hidden = index !== slideIndex/);
  assert.match(html, /data\.status === 'navigate'/);
  assert.match(html, /viewport\.addEventListener\('wheel'/);
  assert.match(html, /new ResizeObserver\(applyState\)/);
  assert.match(html, /const fitScale = Math\.min/);
  assert.match(html, /#ace-presentation-viewport \{ position: relative; width: 100%; height: 100%; overflow: auto;/);
  assert.match(html, /#ace-presentation-root > \.pptx-wrapper \{ width: 100%; transform: none !important;/);
  assert.match(html, /#ace-presentation-root section\[hidden\] \{ display: none !important;/);
});

run('presentation messages require the exact frame, source, channel, and status', () => {
  const frameWindow = {};
  const valid = {
    source: frameWindow,
    data: {
      source: PRESENTATION_PREVIEW_SOURCE,
      channel: 'channel-1',
      status: 'complete',
    },
  };
  assert.deepEqual(presentationPreviewMessage(valid, frameWindow, 'channel-1'), {
    status: 'complete',
    detail: '',
  });
  assert.equal(presentationPreviewMessage({ ...valid, source: {} }, frameWindow, 'channel-1'), null);
  assert.equal(presentationPreviewMessage({ ...valid, data: { ...valid.data, channel: 'other' } }, frameWindow, 'channel-1'), null);
  assert.equal(presentationPreviewMessage({ ...valid, data: { ...valid.data, status: 'render' } }, frameWindow, 'channel-1'), null);
});

run('presentation state messages expose only validated slide and zoom values', () => {
  const frameWindow = {};
  const event = {
    source: frameWindow,
    data: {
      source: PRESENTATION_PREVIEW_SOURCE,
      channel: 'channel-1',
      status: 'state',
      slideIndex: 2,
      slideCount: 5,
      zoom: 1.3,
    },
  };
  assert.deepEqual(presentationPreviewMessage(event, frameWindow, 'channel-1'), {
    status: 'state',
    detail: '',
    slideIndex: 2,
    slideCount: 5,
    zoom: 1.3,
  });
  assert.deepEqual(presentationPreviewMessage({
    ...event,
    data: { ...event.data, slideIndex: -1, slideCount: '5', zoom: 9 },
  }, frameWindow, 'channel-1'), {
    status: 'state',
    detail: '',
  });
});

run('presentation render messages transfer the supplied ArrayBuffer', () => {
  const calls = [];
  const frameWindow = {
    postMessage(...args) {
      calls.push(args);
    },
  };
  const buffer = new ArrayBuffer(8);
  assert.equal(postPresentationPreviewMessage(
    frameWindow,
    'channel-1',
    'render',
    { buffer },
    [buffer],
  ), true);
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0].buffer, buffer);
  assert.deepEqual(calls[0][2], [buffer]);
});

run('presentation preview stays sandboxed and fills the detail pane', () => {
  const component = fs.readFileSync(new URL('../components/PresentationPreview.jsx', import.meta.url), 'utf8');
  const filePreview = fs.readFileSync(new URL('../components/FilePreviewContent.jsx', import.meta.url), 'utf8');
  const styles = fs.readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
  assert.match(component, /pptx2html\/dist\/pptx2html\.full\.min\.js\?raw/);
  assert.match(component, /presentationRendererSource\(rawRendererSource\)/);
  assert.match(component, /sandbox="allow-scripts"/);
  assert.doesNotMatch(component, /allow-same-origin/);
  assert.match(component, /removeEventListener\('message', receiveMessage\)/);
  assert.match(component, /<OfficePreviewControls/);
  assert.match(component, /ace-side-presentation-nav is-previous/);
  assert.match(component, /ace-side-presentation-nav is-next/);
  assert.match(component, /'navigate'/);
  assert.match(filePreview, /state\.kind === 'presentation'/);
  assert.match(styles, /\.ace-side-presentation-preview[\s\S]*flex:\s*1 1 auto/);
  assert.match(styles, /\.ace-side-presentation-frame[\s\S]*height:\s*100%/);
  assert.match(styles, /\.ace-side-presentation-nav\.is-previous[\s\S]*linear-gradient/);
  assert.match(styles, /\.ace-side-presentation-nav\.is-next[\s\S]*linear-gradient/);
});
