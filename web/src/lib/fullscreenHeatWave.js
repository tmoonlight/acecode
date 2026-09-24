import { HEAT_DURATION, MAP_RANGE } from './heatWaveShader.js';
import { createHeatWaveRenderer } from './heatWaveRenderer.js';

export function isFullscreenHeatWaveShortcut(event) {
  return !!(event && event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey
    && !event.isComposing && event.keyCode !== 229
    && (event.code === 'KeyO' || event.key?.toLowerCase() === 'o'));
}

const smooth = (value) => {
  const t = Math.max(0, Math.min(1, value));
  return t * t * (3 - 2 * t);
};

// The demo's complete-test timeline, independent of the resting border's shader clock.
export function sampleFullscreenHeatWave(elapsed, reducedMotion = false) {
  const time = Math.max(0, Number(elapsed) || 0);
  const fade = reducedMotion ? 0.18 : 0.65;
  const waveAt = fade + 0.25;
  const closeAt = waveAt + HEAT_DURATION + 0.45;
  const waveAge = time - waveAt;
  const wave = waveAge >= 0 && waveAge < HEAT_DURATION;
  return {
    amount: smooth(time / fade) * (1 - smooth((time - closeAt) / fade)),
    shaderTime: reducedMotion ? 0 : time,
    motion: reducedMotion ? 0.4 : 1,
    waveAge,
    wave,
    complete: time >= closeAt + fade,
    fps: wave || time < fade || time >= closeAt ? 60 : 30,
  };
}

let activeStart = () => {};

// Same play() the Ctrl+O keydown path starts. Safe to call before install.
export function startFullscreenHeatWave() {
  activeStart();
}

export function installFullscreenHeatWave(target = window) {
  const doc = target.document;
  let current = null;
  let pageHidden = false;

  function clearWarp(run) {
    if (run.priorFilter) {
      // Restore only the property we own; do not overwrite an intervening style change.
      if (doc.body.style.getPropertyValue('filter') === run.appliedFilter) {
        const { value, priority } = run.priorFilter;
        if (value) doc.body.style.setProperty('filter', value, priority);
        else doc.body.style.removeProperty('filter');
      }
      run.priorFilter = null;
    }
    run.mapImage.removeAttribute('href');
  }

  function stop() {
    const run = current;
    if (!run) return;
    current = null;
    if (run.frame) target.cancelAnimationFrame(run.frame);
    clearWarp(run);
    for (const canvas of [run.heat, run.glow]) {
      canvas.removeEventListener('webglcontextlost', onContextLost);
      canvas.remove();
    }
    for (const renderer of run.renderers) renderer.dispose();
    run.svg.remove();
  }

  function onContextLost(event) {
    event.preventDefault();
    stop();
  }

  function requestFrame() {
    if (current && !current.frame && !doc.hidden && !pageHidden) {
      current.frame = target.requestAnimationFrame(render);
    }
  }

  function resize(run) {
    const w = Math.max(1, doc.documentElement.clientWidth);
    const h = Math.max(1, doc.documentElement.clientHeight);
    const short = Math.min(w, h);
    const ratio = Math.min(target.devicePixelRatio || 1, 1.6, Math.sqrt(2000000 / (w * h)));
    const key = `${w}:${h}:${ratio}`;
    if (run.sizeKey !== key) {
      run.sizeKey = key;
      run.glow.width = Math.max(1, Math.round(w * ratio));
      run.glow.height = Math.max(1, Math.round(h * ratio));
      const mapRatio = Math.min(144 / short, 384 / Math.max(w, h));
      run.heat.width = Math.max(1, Math.round(w * mapRatio));
      run.heat.height = Math.max(1, Math.round(h * mapRatio));
      for (const node of [run.filter, run.mapImage]) {
        node.setAttribute('width', w);
        node.setAttribute('height', h);
      }
    }
    return { w, h, short };
  }

  function render(now) {
    const run = current;
    if (!run) return;
    run.frame = 0;
    if (doc.hidden || pageHidden) return;
    if (now < run.nextRenderAt) { requestFrame(); return; }
    if (run.lastTime !== null) run.elapsed += Math.min((now - run.lastTime) / 1000, 0.1);
    run.lastTime = now;
    const state = sampleFullscreenHeatWave(run.elapsed, run.reducedMotion);
    if (state.complete) { stop(); return; }
    try {
      const size = resize(run);
      if (state.wave) {
        // Reuse the small canvas: encode displacement, then draw the visible light.
        run.renderers[1].draw(size, state);
        run.mapImage.setAttribute('href', run.heat.toDataURL('image/png'));
        run.displacement.setAttribute('scale', size.short * MAP_RANGE * state.amount);
        if (!run.priorFilter) {
          run.priorFilter = {
            value: doc.body.style.getPropertyValue('filter'),
            priority: doc.body.style.getPropertyPriority('filter'),
          };
          const base = target.getComputedStyle(doc.body).filter;
          doc.body.style.setProperty('filter', `${base === 'none' ? '' : `${base} `}url("#ace-fullscreen-heat-warp")`, run.priorFilter.priority);
          run.appliedFilter = doc.body.style.getPropertyValue('filter');
        }
        run.renderers[1].draw(size, state, 2);
      } else if (run.priorFilter) {
        clearWarp(run);
      }
      run.heat.style.opacity = state.wave ? '1' : '0';
      run.renderers[0].draw(size, state);
      run.nextRenderAt = now + 1000 / state.fps - 0.5;
      requestFrame();
    } catch (error) {
      stop();
      console.warn('Fullscreen heat wave stopped:', error);
    }
  }

  function play() {
    if (current || doc.hidden || pageHidden) return;
    const makeCanvas = (layer) => {
      const canvas = doc.createElement('canvas');
      canvas.className = 'ace-fullscreen-heat-layer';
      canvas.dataset.aceHeatLayer = layer;
      canvas.setAttribute('popover', 'manual');
      canvas.setAttribute('aria-hidden', 'true');
      return canvas;
    };
    const heat = makeCanvas('heat'), glow = makeCanvas('glow');
    // Top-layer compositing keeps the lights out of body's refraction without
    // reparenting React roots/portals or changing editor selection and focus.
    if (typeof heat.showPopover !== 'function') return;
    const svg = doc.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '0');
    svg.setAttribute('height', '0');
    svg.setAttribute('aria-hidden', 'true');
    svg.style.cssText = 'position:fixed;pointer-events:none';
    svg.innerHTML = `<defs>
      <filter id="ace-fullscreen-heat-warp" filterUnits="userSpaceOnUse" x="0" y="0" color-interpolation-filters="sRGB">
        <feImage x="0" y="0" preserveAspectRatio="none" result="encoded-map"/>
        <feComponentTransfer in="encoded-map" result="map">
          <feFuncR type="linear" slope="1" intercept="-0.00196078431372549"/>
          <feFuncG type="linear" slope="1" intercept="-0.00196078431372549"/>
        </feComponentTransfer>
        <feDisplacementMap in="SourceGraphic" in2="map" scale="0" xChannelSelector="R" yChannelSelector="G"/>
      </filter>
    </defs>`;
    const run = {
      heat, glow, svg, renderers: [], frame: 0, elapsed: 0, lastTime: null, nextRenderAt: 0,
      reducedMotion: target.matchMedia('(prefers-reduced-motion: reduce)').matches,
      filter: svg.querySelector('filter'), mapImage: svg.querySelector('feImage'),
      displacement: svg.querySelector('feDisplacementMap'), priorFilter: null,
    };
    current = run;
    try {
      run.renderers.push(createHeatWaveRenderer(glow, 0));
      run.renderers.push(createHeatWaveRenderer(heat, 1));
      heat.style.opacity = '0';
      doc.body.append(svg, heat, glow);
      for (const canvas of [heat, glow]) {
        canvas.addEventListener('webglcontextlost', onContextLost);
        canvas.showPopover();
      }
      requestFrame();
    } catch (error) {
      stop();
      console.warn('Fullscreen heat wave unavailable:', error);
    }
  }

  function onKeyDown(event) {
    if (!isFullscreenHeatWaveShortcut(event)) return;
    event.preventDefault();
    event.stopPropagation();
    if (!event.repeat) startFullscreenHeatWave();
  }
  function suspend() {
    if (!current) return;
    if (current.frame) target.cancelAnimationFrame(current.frame);
    current.frame = 0;
    current.lastTime = null;
    current.nextRenderAt = 0;
  }
  const onVisibility = () => { suspend(); if (!doc.hidden) requestFrame(); };
  const onPageHide = () => { pageHidden = true; suspend(); };
  const onPageShow = () => { pageHidden = false; requestFrame(); };
  const previousStart = activeStart;
  activeStart = play;
  target.addEventListener('keydown', onKeyDown, true);
  doc.addEventListener('visibilitychange', onVisibility);
  target.addEventListener('pagehide', onPageHide);
  target.addEventListener('pageshow', onPageShow);
  return () => {
    target.removeEventListener('keydown', onKeyDown, true);
    doc.removeEventListener('visibilitychange', onVisibility);
    target.removeEventListener('pagehide', onPageHide);
    target.removeEventListener('pageshow', onPageShow);
    stop();
    if (activeStart === play) activeStart = previousStart;
  };
}
