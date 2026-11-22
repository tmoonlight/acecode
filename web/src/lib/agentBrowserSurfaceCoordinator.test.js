import assert from 'node:assert/strict';
import { agentBrowserLayoutFromRect } from './agentBrowser.js';
import {
  agentBrowserLayoutSignature,
  allocateAgentBrowserLayoutRevision,
  clientRectsOverlap,
  expandClientRect,
  failedNativeSurfaceOcclusionSignature,
  intersectClientRects,
  nativeSurfaceInteractionBlockedByDocument,
  nativeSurfaceOcclusionRectsFromClientRects,
  nativeSurfaceLayoutWithOcclusionFallback,
  nativeSurfaceOverlayGeometryByDocument,
  nativeSurfaceOverlayBlocks,
  nativeSurfaceShouldShow,
  nativeSurfaceSupportsLocalOcclusion,
  nextAgentBrowserLayoutRequest,
  overlayBleed,
  occlusionSquareCorners,
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

run('overlay bleed expands native occlusion holes so borders are not clipped', () => {
  assert.deepEqual(expandClientRect({ left: 10, top: 20, width: 100, height: 40 }, 3), {
    left: 7,
    top: 17,
    right: 113,
    bottom: 63,
    width: 106,
    height: 46,
  });
  assert.deepEqual(expandClientRect({ left: 10, top: 20, width: 100, height: 40 }, 0), {
    left: 10,
    top: 20,
    right: 110,
    bottom: 60,
    width: 100,
    height: 40,
  });
  // Bleed is clamped to [0, 8] and rounded to whole CSS pixels.
  assert.deepEqual(expandClientRect({ left: 0, top: 0, width: 10, height: 10 }, 99), {
    left: -8, top: -8, right: 18, bottom: 18, width: 26, height: 26,
  });
  assert.equal(overlayBleed({ getAttribute: () => '3' }), 3);
  assert.equal(overlayBleed({ getAttribute: () => '99' }), 8);
  assert.equal(overlayBleed({ getAttribute: () => null }), 0);
  // Menus, listboxes and tooltips keep their outline by default: the native
  // region cut is not antialiased, so one extra CSS pixel of bleed keeps the
  // outline inside the hole after DPI scaling.
  const menu = {
    getAttribute: () => null,
    matches: (selector) => selector.includes('[role="menu"]'),
  };
  assert.equal(overlayBleed(menu), 1);
  assert.equal(overlayBleed({
    ...menu,
    getAttribute: (name) => (name === 'data-ace-native-overlay-bleed' ? '0' : null),
  }), 0);
});

run('registered bleed grows overlap occlusion rect and radius', () => {
  const overlay = {
    hidden: false,
    getAttribute: (name) => ({
      'data-ace-native-overlay': 'overlap',
      'data-ace-native-overlay-bleed': '3',
    })[name] ?? null,
    getBoundingClientRect: () => ({ left: 900, top: 300, width: 216, height: 300 }),
  };
  const doc = { querySelectorAll: () => [overlay] };
  const win = {
    getComputedStyle: () => ({
      display: 'block',
      visibility: 'visible',
      opacity: '1',
      pointerEvents: 'auto',
      borderTopLeftRadius: '16px',
      borderTopRightRadius: '16px',
      borderBottomRightRadius: '16px',
      borderBottomLeftRadius: '16px',
    }),
  };
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects, [{
    left: 897,
    top: 297,
    right: 1119,
    bottom: 603,
    width: 222,
    height: 306,
    radius: 19,
  }]);
});

run('surface-clipped hole corners stay square instead of curving through the overlay', () => {
  const menuGeometry = (rect) => {
    const overlay = {
      hidden: false,
      getAttribute: (name) => (name === 'data-ace-native-overlay' ? 'overlap' : null),
      getBoundingClientRect: () => rect,
    };
    const doc = { querySelectorAll: () => [overlay] };
    const win = {
      getComputedStyle: () => ({
        display: 'block',
        visibility: 'visible',
        opacity: '1',
        pointerEvents: 'auto',
        borderTopLeftRadius: '10px',
        borderTopRightRadius: '10px',
        borderBottomRightRadius: '10px',
        borderBottomLeftRadius: '10px',
      }),
    };
    return nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects;
  };

  // A tab-strip menu drops below the browser's top edge: the hole is cut by the
  // surface there, so its top corners must not be rounded.
  assert.deepEqual(menuGeometry({ left: 640, top: 60, width: 300, height: 300 }), [{
    left: 640,
    top: 120,
    right: 940,
    bottom: 360,
    width: 300,
    height: 240,
    radius: 10,
    square_corners: 3,
  }]);

  // The same menu overlapping the left edge keeps only its bottom-right corner
  // rounded.
  assert.deepEqual(menuGeometry({ left: 560, top: 60, width: 300, height: 300 }), [{
    left: 600,
    top: 120,
    right: 860,
    bottom: 360,
    width: 260,
    height: 240,
    radius: 10,
    square_corners: 11,
  }]);

  // Fully inside the surface every corner belongs to the overlay itself.
  assert.deepEqual(menuGeometry({ left: 640, top: 200, width: 300, height: 300 }), [{
    left: 640,
    top: 200,
    right: 940,
    bottom: 500,
    width: 300,
    height: 300,
    radius: 10,
  }]);

  assert.equal(occlusionSquareCorners(
    { left: 640, top: 60, width: 300, height: 300 },
    { left: 640, top: 120, width: 300, height: 240 },
  ), 3);
  assert.equal(occlusionSquareCorners(
    { left: 640, top: 200, width: 300, height: 300 },
    { left: 640, top: 200, width: 300, height: 300 },
  ), 0);
});

run('square corners survive surface-pixel scaling and block rectangle merging', () => {
  const clipped = {
    left: 640,
    top: 120,
    right: 940,
    bottom: 360,
    width: 300,
    height: 240,
    radius: 10,
    square_corners: 3,
  };
  assert.deepEqual(
    nativeSurfaceOcclusionRectsFromClientRects(browserRect, [clipped], 2),
    [{ x: 80, y: 0, width: 601, height: 481, radius: 20, square_corners: 3 }],
  );

  const layout = agentBrowserLayoutFromRect(browserRect, 1, true);
  const withCorners = { ...layout, occlusion_rects: [{ x: 80, y: 0, width: 600, height: 480, radius: 20, square_corners: 3 }] };
  const withoutCorners = { ...layout, occlusion_rects: [{ x: 80, y: 0, width: 600, height: 480, radius: 20 }] };
  assert.notEqual(
    agentBrowserLayoutSignature(withCorners),
    agentBrowserLayoutSignature(withoutCorners),
  );

  // Two aligned, equally rounded holes normally merge into one; a clipped
  // corner mask is measured per rectangle and must survive on its own.
  const stacked = nativeSurfaceOcclusionRectsFromClientRects(browserRect, [
    { left: 640, top: 120, width: 300, height: 200, radius: 10, square_corners: 3 },
    { left: 640, top: 320, width: 300, height: 200, radius: 10 },
  ], 1);
  assert.equal(stacked.length, 2);
  assert.equal(stacked[0].square_corners, 3);
  assert.equal(stacked[1].square_corners, undefined);
});

run('native interaction is blocked only by visible overlapping markers', () => {
  const disabled = {
    getBoundingClientRect: () => ({ left: 900, top: 300, width: 216, height: 300 }),
  };
  const outside = {
    hidden: false,
    getBoundingClientRect: () => ({ left: 20, top: 20, width: 100, height: 100 }),
  };
  const hidden = {
    hidden: true,
    getBoundingClientRect: () => ({ left: 900, top: 300, width: 216, height: 300 }),
  };
  const doc = { querySelectorAll: () => [outside, hidden] };
  const win = {
    getComputedStyle: (element) => ({
      display: element.hidden ? 'none' : 'block',
      visibility: 'visible',
      opacity: '1',
      pointerEvents: 'auto',
    }),
  };
  assert.equal(nativeSurfaceInteractionBlockedByDocument(browserRect, doc, win), false);
  doc.querySelectorAll = () => [outside, hidden, disabled];
  assert.equal(nativeSurfaceInteractionBlockedByDocument(browserRect, doc, win), true);
});

run('interaction_blocked participates in layout signature deduplication', () => {
  const base = agentBrowserLayoutFromRect({ left: 600, top: 120, width: 700, height: 500 }, 1, true);
  const unblocked = agentBrowserLayoutSignature(base);
  const blocked = agentBrowserLayoutSignature({ ...base, interaction_blocked: true });
  assert.notEqual(unblocked, blocked);
  assert.equal(blocked, agentBrowserLayoutSignature({ ...base, interaction_blocked: true }));
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
  // Implicit floating surfaces grow by the default 1px bleed so a menu that
  // hangs over the Browser keeps its bottom/right outline.
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win), {
    blocking: false,
    occlusionRects: [{
      left: 699,
      top: 199,
      right: 881,
      bottom: 321,
      width: 182,
      height: 122,
      radius: 1,
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
  ), [{ x: 375, y: 225, width: 271, height: 376 }]);
});

run('occlusion holes snap outward and guard the far edges', () => {
  // 1.25 DPR: the overlay edge at 900.3 CSS px must not round to the nearest
  // pixel, which would drop the column carrying the 1px outline.
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    { left: 0, top: 0, width: 2000, height: 1200 },
    [{ left: 100.3, top: 200.4, width: 50.2, height: 40.7 }],
    1.25,
  ), [{ x: 125, y: 250, width: 65, height: 53 }]);
  // The far edges get one guard pixel: a region is exclusive there, so ending
  // on the overlay's last pixel column erases its outline.
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    { left: 0, top: 0, width: 2000, height: 1200 },
    [{ left: 100, top: 200, width: 50, height: 40 }],
    1,
  ), [{ x: 100, y: 200, width: 51, height: 41 }]);
  // Holes never grow past the Browser surface itself, and an edge the Browser
  // cut stays exactly on the cut.
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    { left: 0, top: 0, width: 300, height: 200 },
    [{ left: 100, top: 50, width: 500, height: 500 }],
    1,
  ), [{ x: 100, y: 50, width: 200, height: 150 }]);
});

run('rounded overlays carry their corner radius into native occlusion holes', () => {
  const overlay = {
    hidden: false,
    getAttribute: (name) => (name === 'data-ace-native-overlay' ? 'overlap' : null),
    getBoundingClientRect: () => ({ left: 900, top: 300, width: 216, height: 300 }),
  };
  const doc = { querySelectorAll: () => [overlay] };
  const win = {
    getComputedStyle: () => ({
      display: 'block',
      visibility: 'visible',
      opacity: '1',
      pointerEvents: 'auto',
      borderTopLeftRadius: '18px',
      borderTopRightRadius: '18px',
      borderBottomRightRadius: '18px',
      borderBottomLeftRadius: '18px',
    }),
  };
  assert.deepEqual(nativeSurfaceOverlayGeometryByDocument(browserRect, doc, win).occlusionRects, [{
    left: 900,
    top: 300,
    right: 1116,
    bottom: 600,
    width: 216,
    height: 300,
    radius: 18,
  }]);
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    browserRect,
    [{ left: 900, top: 300, width: 216, height: 300, radius: 18 }],
    2,
  ), [{ x: 600, y: 360, width: 433, height: 601, radius: 36 }]);
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    browserRect,
    [{ left: 900, top: 300, width: 20, height: 10, radius: 40 }],
    1,
  ), [{ x: 300, y: 180, width: 21, height: 11, radius: 5 }]);
});

run('occlusion holes only merge when their corner radii agree', () => {
  assert.deepEqual(nativeSurfaceOcclusionRectsFromClientRects(
    { left: 0, top: 0, width: 800, height: 600 },
    [
      { left: 10, top: 20, width: 100, height: 40, radius: 8 },
      { left: 110, top: 20, width: 80, height: 40, radius: 0 },
      { left: 10, top: 80, width: 100, height: 40, radius: 8 },
      { left: 110, top: 80, width: 80, height: 40, radius: 8 },
    ],
    1,
  ), [
    { x: 10, y: 20, width: 101, height: 41, radius: 8 },
    { x: 10, y: 80, width: 181, height: 41, radius: 8 },
    { x: 110, y: 20, width: 81, height: 41 },
  ]);
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
    { x: 10, y: 20, width: 181, height: 41 },
    { x: 20, y: 100, width: 61, height: 81 },
    { x: 300, y: 200, width: 101, height: 81 },
    { x: 350, y: 240, width: 101, height: 81 },
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
