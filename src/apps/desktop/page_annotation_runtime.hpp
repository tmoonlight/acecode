#pragma once

namespace acecode::desktop {

inline constexpr const char kPageAnnotationRuntime[] = R"ACEANNOTATION(
((command, createTextPreview, createStyles) => {
  const key = '__acecodePageAnnotationsV1';
  let runtime = globalThis[key];
  if (runtime && command.action === 'start' && runtime.document_id !== command.document_id) {
    runtime.dispose();
    runtime = null;
  }
  if (!runtime && command.action === 'start') {
    const documentId = command.document_id;
    const rootDocument = document;
    const rootElement = document.documentElement;
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
    const draftChains = new Map();
    const ancestors = target => {
      const chain = [];
      for (let node = target; node; node = node.parentNode || node.host) chain.push(node);
      return chain;
    };
    const observed = new WeakSet();
    const textPreview = createTextPreview();
    const styles = createStyles(rootDocument);
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
      draftChains.clear();
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
      text_preview:textPreview.describe(target, ids.get(target)),
      style_preview:styles.describe(target, ids.get(target))});
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
        pin.marker.style.display = !pin.invalid && pin.targets.every(connected) ? 'block' : 'none';
        if (pin.outline) pin.outline.style.display = !pin.invalid && pin.targets.every(connected) ? 'block' : 'none';
        if (pin.invalid || !pin.targets.every(connected)) continue;
        const bounds = pin.region ? viewportRegion(pin.region) : rect(pin.targets[0]);
        pin.marker.style.left = `${bounds.x}px`;
        pin.marker.style.top = `${bounds.y}px`;
        if (pin.outline) place(pin.outline, bounds);
      }
    };
    const lifecycle = () => ({document_id:documentId,
      invalid_numbers:pins.filter(pin => pin.invalid).map(pin => pin.number)});
    const snapshot = () => {
      layout();
      const target = connected(selected) ? summary(selected) : null;
      return {...lifecycle(), active, popup:!!(target || region) && open, target,
        targets:targets.map(summary), region, selection_mode:selectionMode, original,
        differences:[...textPreview.differences(), ...styles.differences()],
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
      if (!targets.includes(target)) {
        targets.push(target);
        draftChains.set(target, ancestors(target));
        observe(target);
      }
      selected = target;
      if (!ids.has(target)) ids.set(target, `target-${++sequence}`);
      selectedId = ids.get(target);
      styles.select(target, selectedId);
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
      styles.cancel();
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
    const removed = (target, chain, records) => !connected(target) || records.some(record =>
      Array.from(record.removedNodes || []).some(node => node === target || chain.includes(node) || node.contains?.(target)));
    const inspect = (records = []) => {
      const invalidIds = new Set();
      for (const pin of pins) {
        if (!pin.invalid && pin.targets.some((target, index) => removed(target, pin.chains[index], records))) {
          pin.invalid = true;
          for (const target of pin.targets) invalidIds.add(ids.get(target));
        }
      }
      if (targets.some(target => removed(target, draftChains.get(target) || ancestors(target), records))) {
        textPreview.cancel();
        styles.cancel();
        clearDraft();
      }
      if (invalidIds.size) {
        textPreview.invalidate?.(invalidIds);
        styles.invalidate?.(invalidIds);
      }
      layout();
    };
    const mutations = typeof MutationObserver === 'function' ? new MutationObserver(inspect) : null;
    const observe = target => {
      for (const node of ancestors(target)) {
        const root = node.getRootNode?.();
        if (root && !observed.has(root)) {
          observed.add(root);
          mutations?.observe(root, {subtree:true, childList:true});
        }
      }
    };
    mutations?.observe(rootDocument, {subtree:true, childList:true});
    const sizes = typeof ResizeObserver === 'function' ? new ResizeObserver(layout) : null;
    sizes?.observe(document.documentElement);
    runtime = Object.freeze({document_id:documentId, dispose() {
      textPreview.dispose();
      styles.dispose();
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
      if (document.documentElement !== rootElement) {
        runtime.dispose();
        return {error:'replaced_document'};
      }
      inspect(mutations?.takeRecords?.() || []);
      switch (next.action) {
        case 'validate_batch':
          return {...lifecycle(), valid:Array.isArray(next.target_ids) && next.target_ids.every(id =>
            pins.some(pin => !pin.invalid && pin.targets.some(target => connected(target) && ids.get(target) === id))) &&
            (!next.record_numbers || next.record_numbers.every(number => pins.some(pin => pin.number === number && !pin.invalid)))};
        case 'start': active = true; break;
        case 'read': break;
        case 'exit': textPreview.cancel(); styles.cancel(); active = false; clearDraft(); break;
        case 'cancel': textPreview.cancel(); styles.cancel(); clearDraft(); break;
        case 'original': original = next.enabled; textPreview.setOriginal(original); styles.setOriginal(original); break;
        case 'preview_style': {
          if (!active || !open || selectionMode || !connected(selected) || selectedId !== next.target_id) return {error:'stale_target'};
          try { styles.preview(selected, selectedId, next.styles, next.ratio_lock); }
          catch (error) { return {error:String(error.message)}; }
          break;
        }
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
          styles.select(target, selectedId);
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
          if (original) return {error:'original_view_active'};
          const result = textPreview.save();
          if (result.error) return result;
          const differences = [...result.differences, ...styles.save()];
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
          for (const target of targets) observe(target);
          pins.push({number:next.number, targets:[...targets], chains:targets.map(ancestors), region, marker, outline:savedOutline, invalid:false});
          clearDraft();
          return {...snapshot(), differences};
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
