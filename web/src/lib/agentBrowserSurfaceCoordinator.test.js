import assert from 'node:assert/strict';
import { agentBrowserLayoutFromRect } from './agentBrowser.js';
import {
  allocateAgentBrowserLayoutRevision,
  clientRectsOverlap,
  failedNativeSurfaceOcclusionSignature,
  intersectClientRects,
  nativeSurfaceOcclusionRectsFromClientRects,
  nativeSurfaceLayoutWithOcclusionFallback,
  nativeSurfaceOverlayGeometryByDocument,
  nativeSurfaceOverlayBlocks,
  nativeSurfaceShouldShow,
  nativeSurfaceSupportsLocalOcclusion,
  nextAgentBrowserLayoutRequest,
} from './agentBrowserSurfaceCoordinator.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const browserRect = { left: 600, top: 120, width: 700, height: 500 };
const viewportRect = { left: 0, top: 0, width: 1600, height: 900 };

run('Windows and macOS native surfaces support local occlusion', () => {
  assert.equal(nativeSurfaceSupportsLocalOcclusion('windows'), true);
  assert.equal(nativeSurfaceSupportsLocalOcclusion('macos'), true);
  assert.equal(nativeSurfaceSupportsLocalOcclusion('linux'), false);
  assert.equal(nativeSurfaceSupportsLocalOcclusion(''), false);
});

run('native surface rectangle helpers distinguish overlap from adjacency', () => {
  assert.equal(clientRectsOverlap(browserRect, {
    left: 100,
    top: 100,
    width: 200,
    height: 200,
  }), false);
  assert.equal(clientRectsOverlap(browserRect, {
    left: 1300,
    top: 150,
    width: 100,
    height: 100,
  }), false);
  assert.deepEqual(intersectClientRects(browserRect, {
    left: 1250,
    top: 550,
    width: 100,
    height: 100,
  }), {
    left: 1250,
    top: 550,
    right: 1300,
    bottom: 620,
    width: 50,
    height: 70,
  });
});

run('overlap-only menus outside the browser do not block it', () => {
  assert.equal(nativeSurfaceOverlayBlocks({
    mode: 'overlap',
    surfaceRect: browserRect,
    overlayRect: { left: 20, top: 20, width: 216, height: 300 },
    topmost: true,
  }), false);
  assert.equal(nativeSurfaceOverlayBlocks({
    mode: 'overlap',
    surfaceRect: browserRect,
    overlayRect: { left: 900, top: 300, width: 216, height: 300 },
    topmost: true,
  }), true);
  assert.equal(nativeSurfaceOverlayBlocks({
    mode: 'blocking',
    surfaceRect: browserRect,
    overlayVisible: true,
  }), true);
});

run('document overlay inspection treats registered floating surfaces as authoritative', () => {
  const child = {};
  const overlay = {
    hidden: false,
    getAttribute: (name) => (name === 'data-ace-native-overlay' ? 'overlap' : null),
    getBoundingClientRect: () => ({ left: 900, top: 300, width: 216, height: 300 }),
    contains: (element) => element === child,
  };
  const doc = {
    querySelectorAll: () => [overlay],
    elementFromPoint: () => child,
  };
  const win = {
    getComputedStyle: () => ({
      display: 'block',
      visibility: 'visible',
      opacity: '1',
      pointerEvents: 'auto',
    }),
  };
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win), {
    blocking: false,
    occlusionRects: [{
      left: 900,
      top: 300,
      right: 1116,
      bottom: 600,
      width: 216,
      height: 300,
    }],
  });
  doc.elementFromPoint = () => ({});
  assert.equal(
    nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects.length,
    1,
  );
  win.getComputedStyle = () => ({
    display: 'block',
    visibility: 'visible',
    opacity: '1',
    pointerEvents: 'none',
  });
  assert.equal(
    nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects.length,
    1,
  );
  overlay.getBoundingClientRect = () => ({ left: 20, top: 20, width: 216, height: 300 });
  assert.deepEqual(
    nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects,
    [],
  );
  overlay.getAttribute = (name) => (name === 'data-ace-native-overlay' ? 'blocking' : null);
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win), {
    blocking: true,
    occlusionRects: [],
  });
});

run('standard floating accessibility roles join the overlay contract automatically', () => {
  const overlay = {
    hidden: false,
    getAttribute: () => null,
    getBoundingClientRect: () => ({ left: 700, top: 200, width: 180, height: 120 }),
    matches: (selector) => selector.includes('[role="menu"]'),
  };
  const doc = { querySelectorAll: () => [overlay] };
  const win = {
    getComputedStyle: () => ({
      display: 'block',
      visibility: 'visible',
      opacity: '1',
      pointerEvents: 'auto',
    }),
  };
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win), {
    blocking: false,
    occlusionRects: [{
      left: 700,
      top: 200,
      right: 880,
      bottom: 320,
      width: 180,
      height: 120,
    }],
  });
});

run('overlap geometry converts to deterministic Browser-local DPR rectangles', () => {
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    browserRect,
    [
      { left: 900, top: 300, width: 216, height: 300 },
      { left: 900, top: 300, width: 216, height: 300 },
      { left: 20, top: 20, width: 100, height: 100 },
    ],
    1.25,
  ), [{ x: 375, y: 225, width: 270, height: 375 }]);
});

run('compatible occlusion rectangles merge without filling uncovered corners', () => {
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    { left: 0, top: 0, width: 800, height: 600 },
    [
      { left: 10, top: 20, width: 100, height: 40 },
      { left: 110, top: 20, width: 80, height: 40 },
      { left: 20, top: 100, width: 60, height: 50 },
      { left: 20, top: 150, width: 60, height: 30 },
      { left: 300, top: 200, width: 100, height: 80 },
      { left: 350, top: 240, width: 100, height: 80 },
    ],
    1,
  ), [
    { x: 10, y: 20, width: 180, height: 40 },
    { x: 20, y: 100, width: 60, height: 80 },
    { x: 300, y: 200, width: 100, height: 80 },
    { x: 350, y: 240, width: 100, height: 80 },
  ]);
});

run('failed current occlusion layouts fail closed until overlay geometry changes', () => {
  const desired = {
    x: 600,
    y: 120,
    width: 700,
    height: 500,
    visible: true,
    occlusion_rects: [{ x: 100, y: 80, width: 180, height: 120 }],
  };
  const initial = nativeSurfaceLayoutWithOcclusionFallback(desired);
  const request = { ...initial.layout, layout_revision: 41 };
  const failedSignature = failedNativeSurfaceOcclusionSignature({
    result: { ok: false, error: 'mask unavailable' },
    request,
    requestSignature: initial.idealSignature,
    currentRevision: 41,
  });
  assert.equal(failedSignature, initial.idealSignature);

  const fallback = nativeSurfaceLayoutWithOcclusionFallback(desired, failedSignature);
  assert.equal(fallback.fallback, true);
  assert.equal(fallback.layout.visible, false);
  assert.deepEqual(fallback.layout.occlusion_rects, []);

  const movedOverlay = nativeSurfaceLayoutWithOcclusionFallback({
    ...desired,
    occlusion_rects: [{ x: 120, y: 80, width: 180, height: 120 }],
  }, failedSignature);
  assert.equal(movedOverlay.fallback, false);
  assert.equal(movedOverlay.layout.visible, true);

  assert.equal(failedNativeSurfaceOcclusionSignature({
    result: { ok: false },
    request,
    requestSignature: initial.idealSignature,
    currentRevision: 42,
  }), '');
  assert.equal(failedNativeSurfaceOcclusionSignature({
    result: { ok: true },
    request,
    requestSignature: initial.idealSignature,
    currentRevision: 41,
  }), '');
});

run('native surface visibility requires every application and geometry gate', () => {
  const visible = {
    applicationVisible: true,
    detailsVisible: true,
    tabActive: true,
    pageLive: true,
    documentVisible: true,
    surfaceRect: browserRect,
    viewportRect,
    overlayBlocked: false,
  };
  assert.equal(nativeSurfaceShouldShow(visible), true);
  for (const key of [
    'applicationVisible',
    'detailsVisible',
    'tabActive',
    'pageLive',
    'documentVisible',
  ]) {
    assert.equal(nativeSurfaceShouldShow({ ...visible, [key]: false }), false, key);
  }
  assert.equal(nativeSurfaceShouldShow({ ...visible, overlayBlocked: true }), false);
  assert.equal(nativeSurfaceShouldShow({
    ...visible,
    surfaceRect: { left: 0, top: 0, width: 0, height: 500 },
  }), false);
  assert.equal(nativeSurfaceShouldShow({
    ...visible,
    surfaceRect: { left: 1800, top: 100, width: 300, height: 300 },
  }), false);
});

run('DPR layout requests are monotonic and deduplicate identical frames', () => {
  const layout = agentBrowserLayoutFromRect({
    left: 10.25,
    top: 20,
    width: 300.5,
    height: 200,
  }, 1.5, true);
  assert.deepEqual(layout, {
    x: 15,
    y: 30,
    width: 451,
    height: 300,
    visible: true,
  });

  const first = nextAgentBrowserLayoutRequest(layout);
  assert.equal(first.changed, true);
  assert.equal(first.request.layout_revision, 1);

  const duplicate = nextAgentBrowserLayoutRequest(layout, first);
  assert.equal(duplicate.changed, false);
  assert.equal(duplicate.request, null);
  assert.equal(duplicate.revision, 1);

  const hidden = nextAgentBrowserLayoutRequest({ ...layout, visible: false }, duplicate);
  assert.equal(hidden.request.layout_revision, 2);
  assert.equal(hidden.request.visible, false);

  const cleanup = nextAgentBrowserLayoutRequest(
    { ...layout, visible: false },
    hidden,
    { force: true },
  );
  assert.equal(cleanup.request.layout_revision, 3);

  const clipped = nextAgentBrowserLayoutRequest({
    ...layout,
    occlusion_rects: [{ x: 20, y: 30, width: 100, height: 80 }],
  }, cleanup);
  assert.equal(clipped.changed, true);
  const reorderedDuplicate = nextAgentBrowserLayoutRequest({
    ...layout,
    occlusion_rects: [
      { x: 20, y: 30, width: 100, height: 80 },
      { x: 20, y: 30, width: 100, height: 80 },
    ],
  }, clipped);
  assert.equal(reorderedDuplicate.changed, false);

  const restored = nextAgentBrowserLayoutRequest({
    ...layout,
    occlusion_rects: [],
  }, clipped);
  assert.equal(restored.changed, true);
  assert.deepEqual(restored.request.occlusion_rects, []);
  assert.ok(restored.revision > clipped.revision);
});

run('layout revisions remain newer after a Browser panel remount', () => {
  const visibleLayout = agentBrowserLayoutFromRect(browserRect, 1.25, true);
  const firstMount = nextAgentBrowserLayoutRequest(visibleLayout, {}, {
    allocateRevision: allocateAgentBrowserLayoutRevision,
  });
  const cleanup = nextAgentBrowserLayoutRequest(
    { ...visibleLayout, visible: false },
    firstMount,
    {
      force: true,
      allocateRevision: allocateAgentBrowserLayoutRevision,
    },
  );
  const remount = nextAgentBrowserLayoutRequest(visibleLayout, {}, {
    allocateRevision: allocateAgentBrowserLayoutRevision,
  });

  assert.ok(Number.isSafeInteger(firstMount.revision));
  assert.ok(cleanup.revision > firstMount.revision);
  assert.ok(remount.revision > cleanup.revision);
});
