#pragma once

namespace acecode::desktop {

inline constexpr const char kPageAnnotationRuntime[] = R"ACEANNOTATION(
((command, createTextPreview) => {
  const key = '__acecodePageAnnotationsV1';
  let runtime = globalThis[key];
  if (runtime && command.action === 'start' && runtime.document_id !== command.document_id) {
    runtime.dispose();
    runtime = null;
  }
  if (!runtime && command.action === 'start') {
    const documentId = command.document_id;
    const rootDocument = document;
    let active = false;
    let selected = null;
    let selectedId = '';
    let open = false;
    let selectionMode = null;
    let region = null;
    let drag = null;
    const targets = [];
    let sequence = 0;
    const ids = new WeakMap();
    const pins = [];
    const textPreview = createTextPreview();
    let original = false;
    const host = document.createElement('div');
    host.style.cssText = 'all:initial;position:fixed;inset:0;pointer-events:none;z-index:2147483647';
    const shadow = host.attachShadow({mode:'closed'});
    const outline = document.createElement('div');
    outline.style.cssText = 'position:fixed;border:2px solid Highlight;box-sizing:border-box;pointer-events:none;display:none';
    const control = document.createElement('button');
    control.type = 'button';
    control.textContent = 'Annotate';
    control.setAttribute('aria-label', 'Annotate selected element');
    control.style.cssText = 'position:fixed;pointer-events:auto;background:ButtonFace;color:ButtonText;border:1px solid ButtonBorder;border-radius:4px;padding:4px 8px;display:none';
    shadow.append(outline, control);
    const regionOutline = document.createElement('div');
    regionOutline.style.cssText = 'position:fixed;border:2px dashed Highlight;box-sizing:border-box;pointer-events:none;display:none';
    shadow.append(regionOutline);
    document.documentElement.append(host);
    const connected = target => target?.isConnected && target.ownerDocument === rootDocument;
    const rect = target => {
      const bounds = target.getBoundingClientRect();
      return {x:bounds.x, y:bounds.y, width:bounds.width, height:bounds.height};
    };
    const pagePoint = event => ({x:Math.max(0, event.clientX + scrollX), y:Math.max(0, event.clientY + scrollY)});
    const regionRect = (first, last) => ({x:Math.min(first.x, last.x), y:Math.min(first.y, last.y),
      width:Math.abs(last.x - first.x), height:Math.abs(last.y - first.y)});
    const viewportRegion = bounds => ({...bounds, x:bounds.x - scrollX, y:bounds.y - scrollY});
    const place = (marker, bounds) => Object.assign(marker.style,
      {left:`${bounds.x}px`, top:`${bounds.y}px`, width:`${bounds.width}px`, height:`${bounds.height}px`});
    const clearDraft = () => {
      targets.length = 0;
      selected = null;
      selectedId = '';
      region = null;
      selectionMode = null;
      drag = null;
      open = false;
    };
    const summary = target => ({id:ids.get(target), tag:target.localName.slice(0, 64),
      text:(target.innerText || target.textContent || '').slice(0, 512),
      locator:target.localName + (target.id ? '#' + target.id.slice(0, 128) : ''), bounds:rect(target),
      text_preview:textPreview.describe(target, ids.get(target))});
    const layout = () => {
      const visible = active && connected(selected) && selectionMode !== 'region';
      outline.style.display = visible ? 'block' : 'none';
      control.style.display = visible && !open ? 'block' : 'none';
      if (visible) {
        const bounds = rect(selected);
        Object.assign(outline.style, {left:`${bounds.x}px`, top:`${bounds.y}px`, width:`${bounds.width}px`, height:`${bounds.height}px`});
        control.style.left = `${Math.max(0, Math.min(innerWidth - control.offsetWidth, bounds.x))}px`;
        control.style.top = `${Math.max(0, Math.min(innerHeight - control.offsetHeight, bounds.y + bounds.height))}px`;
      }
      const displayedRegion = drag ? regionRect(drag.start, drag.end) : region;
      regionOutline.style.display = active && displayedRegion ? 'block' : 'none';
      if (displayedRegion) place(regionOutline, viewportRegion(displayedRegion));
      for (const pin of pins) {
        pin.marker.style.display = pin.targets.every(connected) ? 'block' : 'none';
        if (pin.outline) pin.outline.style.display = pin.targets.every(connected) ? 'block' : 'none';
        if (!pin.targets.every(connected)) continue;
        const bounds = pin.region ? viewportRegion(pin.region) : rect(pin.targets[0]);
        pin.marker.style.left = `${bounds.x}px`;
        pin.marker.style.top = `${bounds.y}px`;
        if (pin.outline) place(pin.outline, bounds);
      }
    };
    const snapshot = () => {
      layout();
      const target = connected(selected) ? summary(selected) : null;
      return {document_id:documentId, active, popup:!!(target || region) && open, target,
        targets:targets.map(summary), region, selection_mode:selectionMode, original,
        viewport:{width:innerWidth, height:innerHeight, scroll_x:scrollX, scroll_y:scrollY}};
    };
    const select = event => {
      if (!active || !event.isTrusted || event.composedPath().includes(host)) return;
      if (event.button !== 0) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (open && selectionMode !== 'element') return;
      const target = event.composedPath().find(node => node?.nodeType === 1);
      if (!connected(target)) return;
      if (!targets.includes(target) && targets.length >= 64) return;
      if (!open) targets.length = 0;
      if (!targets.includes(target)) targets.push(target);
      selected = target;
      if (!ids.has(target)) ids.set(target, `target-${++sequence}`);
      selectedId = ids.get(target);
      selectionMode = null;
      layout();
    };
    const pointerDown = event => {
      if (selectionMode !== 'region') { suppress(event); return; }
      if (!active || !event.isTrusted || event.button !== 0 || event.composedPath().includes(host)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      drag = {start:pagePoint(event), end:pagePoint(event), pointerId:event.pointerId};
      host.setPointerCapture?.(event.pointerId);
      layout();
    };
    const pointerMove = event => {
      if (!drag || !event.isTrusted || event.pointerId !== drag.pointerId) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      drag.end = pagePoint(event);
      layout();
    };
    const pointerUp = event => {
      if (!drag || !event.isTrusted || event.pointerId !== drag.pointerId) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      const next = regionRect(drag.start, pagePoint(event));
      drag = null;
      if (next.width > 0 && next.height > 0) {
        region = next;
        selected = null;
        selectedId = '';
        selectionMode = null;
      }
      layout();
    };
    const pointerCancel = event => {
      if (!drag || !event.isTrusted || event.pointerId !== drag.pointerId) return;
      drag = null;
      layout();
    };
    const suppress = event => {
      if (active && event.isTrusted && !event.composedPath().includes(host)) {
        event.preventDefault();
        event.stopImmediatePropagation();
      }
    };
    const escape = event => {
      if (!active || !event.isTrusted || event.key !== 'Escape') return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (selectionMode) {
        selectionMode = null;
        drag = null;
        layout();
        return;
      }
      active = false;
      textPreview.cancel();
      clearDraft();
      layout();
    };
    control.addEventListener('click', event => {
      if (!active || !event.isTrusted || !connected(selected)) return;
      event.preventDefault();
      event.stopPropagation();
      open = true;
      layout();
    });
    document.addEventListener('pointerdown', pointerDown, true);
    document.addEventListener('pointermove', pointerMove, true);
    document.addEventListener('pointerup', pointerUp, true);
    document.addEventListener('pointercancel', pointerCancel, true);
    document.addEventListener('mousedown', suppress, true);
    document.addEventListener('click', select, true);
    document.addEventListener('keydown', escape, true);
    globalThis.addEventListener('scroll', layout, true);
    globalThis.addEventListener('resize', layout);
    const mutations = typeof MutationObserver === 'function' ? new MutationObserver(layout) : null;
    mutations?.observe(document.documentElement, {subtree:true, childList:true, attributes:true, characterData:true});
    const sizes = typeof ResizeObserver === 'function' ? new ResizeObserver(layout) : null;
    sizes?.observe(document.documentElement);
    runtime = Object.freeze({document_id:documentId, dispose() {
      textPreview.dispose();
      mutations?.disconnect();
      sizes?.disconnect();
      document.removeEventListener('pointerdown', pointerDown, true);
      document.removeEventListener('pointermove', pointerMove, true);
      document.removeEventListener('pointerup', pointerUp, true);
      document.removeEventListener('pointercancel', pointerCancel, true);
      document.removeEventListener('mousedown', suppress, true);
      document.removeEventListener('click', select, true);
      document.removeEventListener('keydown', escape, true);
      globalThis.removeEventListener('scroll', layout, true);
      globalThis.removeEventListener('resize', layout);
      host.remove();
    }, run(next) {
      if (document !== rootDocument || next.document_id !== documentId) return {error:'stale_document'};
      switch (next.action) {
        case 'start': active = true; break;
        case 'read': break;
        case 'exit': textPreview.cancel(); active = false; clearDraft(); break;
        case 'cancel': textPreview.cancel(); clearDraft(); break;
        case 'original': original = next.enabled; textPreview.setOriginal(original); break;
        case 'preview_text': {
          if (!active || !open || selectionMode || !connected(selected) || selectedId !== next.target_id) return {error:'stale_target'};
          const result = textPreview.preview(selected, selectedId, next.text);
          if (result.error) return result;
          break;
        }
        case 'add_target':
        case 'add_region':
          if (!active || !open || selectionMode) return {error:'stale_target'};
          selectionMode = next.action === 'add_target' ? 'element' : 'region';
          break;
        case 'select_target': {
          const target = targets.find(item => ids.get(item) === next.target_id);
          if (!active || !open || selectionMode || !connected(target)) return {error:'stale_target'};
          selected = target;
          selectedId = ids.get(target);
          break;
        }
        case 'select_region':
          if (!active || !open || selectionMode || !region) return {error:'stale_target'};
          selected = null;
          selectedId = '';
          break;
        case 'save': {
          if (!active || !open || selectionMode || !targets.every(connected) ||
              (!selected && !region) || (selected ? selectedId : null) !== next.target_id ||
              JSON.stringify(targets.map(target => ids.get(target))) !== JSON.stringify(next.target_ids) ||
              (region ? !next.region || ['x','y','width','height'].some(field => region[field] !== next.region[field])
                : next.region !== null)) return {error:'stale_target'};
          const result = textPreview.save();
          if (result.error) return result;
          const marker = document.createElement('span');
          marker.textContent = String(next.number);
          marker.style.cssText = 'position:fixed;pointer-events:none;border-radius:12px;padding:2px 6px;background:Highlight;color:HighlightText';
          shadow.append(marker);
          let savedOutline = null;
          if (region) {
            savedOutline = document.createElement('div');
            savedOutline.style.cssText = regionOutline.style.cssText;
            savedOutline.style.display = 'block';
            shadow.append(savedOutline);
          }
          pins.push({targets:[...targets], region, marker, outline:savedOutline});
          clearDraft();
          return {...snapshot(), differences:result.differences};
        }
        default: return {error:'unsupported_action'};
      }
      return snapshot();
    }});
    Object.defineProperty(globalThis, key, {value:runtime, configurable:true});
  }
  if (!runtime) return {error:'stale_document'};
  return runtime.run(command);
})
)ACEANNOTATION";

} // namespace acecode::desktop
