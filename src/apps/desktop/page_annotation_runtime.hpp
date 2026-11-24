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
    let hovered = null;
    let modalSuspended = false;
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
    let markersHidden = false;
    const host = document.createElement('div');
    host.style.cssText = 'all:initial;position:fixed;inset:0;pointer-events:none;z-index:2147483647';
    host.setAttribute('data-ace-annotation-overlay', '');
    const shadow = host.attachShadow({mode:'closed'});
    const outline = document.createElement('div');
    outline.style.cssText = 'position:fixed;border:2px solid Highlight;box-sizing:border-box;pointer-events:none;display:none';
    const hoverOutline = document.createElement('div');
    hoverOutline.style.cssText = 'position:fixed;border:1px solid Highlight;border-radius:2px;box-sizing:border-box;pointer-events:none;display:none';
    hoverOutline.setAttribute('data-ace-annotation-hover', '');
    const infoCard = document.createElement('div');
    infoCard.style.cssText = 'position:fixed;pointer-events:none;display:none;box-sizing:border-box;min-width:168px;max-width:260px;padding:8px 10px;border-radius:10px;background:#fff;color:#1f2328;font:12px/1.5 ui-sans-serif,system-ui,sans-serif;border:1px solid rgba(0,0,0,.08);box-shadow:0 8px 24px rgba(0,0,0,.18)';
    infoCard.setAttribute('data-ace-annotation-info', '');
    shadow.append(outline, hoverOutline, infoCard);
    const regionOutline = document.createElement('div');
    regionOutline.style.cssText = 'position:fixed;border:2px dashed Highlight;box-sizing:border-box;pointer-events:none;display:none';
    shadow.append(regionOutline);
    document.documentElement.append(host);
    const connected = target => target?.isConnected && target.ownerDocument === rootDocument;
    const rect = target => {
      const bounds = target.getBoundingClientRect();
      return {x:bounds.x, y:bounds.y, width:bounds.width, height:bounds.height};
    };
    const pageBackground = () => {
      for (const node of [document.body, document.documentElement]) {
        if (!node) continue;
        const color = getComputedStyle(node).backgroundColor;
        if (color && color !== 'transparent' && color !== 'rgba(0, 0, 0, 0)') return color;
      }
      return '#ffffff';
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
      hovered = null;
      open = false;
    };
    const clearPins = () => {
      for (const pin of pins) {
        pin.marker.remove();
        pin.outline?.remove();
      }
      pins.length = 0;
    };
    const summary = target => ({id:ids.get(target), tag:target.localName.slice(0, 64),
      text:(target.innerText || target.textContent || '').slice(0, 512),
      locator:target.localName + (target.id ? '#' + target.id.slice(0, 128) : ''), bounds:rect(target),
      text_preview:textPreview.describe(target, ids.get(target)),
      style_preview:styles.describe(target, ids.get(target))});
    const hexColor = value => {
      const match = /^rgba?\(([^)]+)\)$/i.exec(String(value || ''));
      if (!match) return String(value || '');
      const parts = match[1].split(',').map(part => Number(part.trim()));
      if (parts.length < 3 || parts.slice(0, 3).some(Number.isNaN)) return String(value || '');
      if (parts.length > 3 && parts[3] === 0) return 'transparent';
      return '#' + parts.slice(0, 3).map(part =>
        Math.max(0, Math.min(255, Math.round(part))).toString(16).padStart(2, '0')).join('');
    };
    const infoLine = (label, value, swatch) => {
      const row = document.createElement('div');
      row.style.cssText = 'display:grid;grid-template-columns:36px minmax(0,1fr);align-items:center;gap:8px;min-height:18px';
      const name = document.createElement('span');
      name.textContent = label;
      name.style.cssText = 'color:#8b949e;font-size:11px';
      const content = document.createElement('span');
      content.style.cssText = 'display:flex;align-items:center;gap:6px;min-width:0;overflow:hidden;white-space:nowrap;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:11px;color:#57606a';
      if (swatch) {
        const dot = document.createElement('span');
        dot.style.cssText = `flex:0 0 auto;width:10px;height:10px;border-radius:3px;border:1px solid rgba(0,0,0,.12);background:${swatch}`;
        content.append(dot);
      }
      const text = document.createElement('span');
      text.textContent = value;
      text.style.cssText = 'min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap';
      content.append(text);
      row.append(name, content);
      return row;
    };
    const renderInfo = target => {
      const computedStyle = getComputedStyle(target);
      const bounds = rect(target);
      infoCard.replaceChildren();
      const head = document.createElement('div');
      head.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:5px';
      const tag = document.createElement('span');
      tag.textContent = target.localName.slice(0, 64);
      tag.style.cssText = 'font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:11px;font-weight:600;color:#1f2328';
      const size = document.createElement('span');
      size.textContent = `${Math.round(bounds.width)} x ${Math.round(bounds.height)}`;
      size.style.cssText = 'font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:11px;color:#8b949e';
      head.append(tag, size);
      infoCard.append(head,
        infoLine('颜色', hexColor(computedStyle.color), computedStyle.color),
        infoLine('字体', `${computedStyle.fontSize} ${computedStyle.fontFamily}`, null));
    };
)ACEANNOTATION" R"ACEANNOTATION(
    const layout = () => {
      const visible = !modalSuspended && active && connected(selected) && selectionMode !== 'region';
      outline.style.display = visible ? 'block' : 'none';
      if (visible) {
        const bounds = rect(selected);
        Object.assign(outline.style, {left:`${bounds.x}px`, top:`${bounds.y}px`, width:`${bounds.width}px`, height:`${bounds.height}px`});
      }
      const hovering = !modalSuspended && active && connected(hovered) && !drag && selectionMode !== 'region' &&
        (!open || selectionMode === 'element');
      hoverOutline.style.display = hovering ? 'block' : 'none';
      infoCard.style.display = hovering ? 'block' : 'none';
      if (hovering) {
        const bounds = rect(hovered);
        Object.assign(hoverOutline.style, {left:`${bounds.x}px`, top:`${bounds.y}px`, width:`${bounds.width}px`, height:`${bounds.height}px`});
        const cardWidth = infoCard.offsetWidth;
        const cardHeight = infoCard.offsetHeight;
        infoCard.style.left = `${Math.max(8, Math.min(innerWidth - cardWidth - 8, bounds.x))}px`;
        const below = bounds.y + bounds.height + 6;
        infoCard.style.top = `${below + cardHeight <= innerHeight - 8
          ? below : Math.max(8, bounds.y - cardHeight - 6)}px`;
      }
      const displayedRegion = drag ? regionRect(drag.start, drag.end) : region;
      regionOutline.style.display = !modalSuspended && active && displayedRegion ? 'block' : 'none';
      if (displayedRegion) place(regionOutline, viewportRegion(displayedRegion));
      const markersVisible = !markersHidden && !original;
      for (const pin of pins) {
        const visiblePin = markersVisible && !pin.invalid && pin.targets.every(connected);
        pin.marker.style.display = visiblePin ? 'block' : 'none';
        if (pin.outline) pin.outline.style.display = visiblePin ? 'block' : 'none';
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
        markers_hidden:markersHidden, modal_suspended:modalSuspended,
        differences:[...textPreview.differences(), ...styles.differences()],
        page_background:pageBackground(),
        viewport:{width:innerWidth, height:innerHeight, scroll_x:scrollX, scroll_y:scrollY}};
    };
    const select = event => {
      if (!active || modalSuspended || !event.isTrusted || event.composedPath().includes(host)) return;
      if (event.button !== 0) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (open && selectionMode !== 'element') {
        textPreview.cancel();
        styles.cancel();
        clearDraft();
        layout();
        return;
      }
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
      open = true;
      layout();
    };
    const pointerDown = event => {
      if (selectionMode !== 'region') { suppress(event); return; }
      if (!active || modalSuspended || !event.isTrusted || event.button !== 0 || event.composedPath().includes(host)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      drag = {start:pagePoint(event), end:pagePoint(event), pointerId:event.pointerId};
      host.setPointerCapture?.(event.pointerId);
      layout();
    };
    const pointerMove = event => {
      if (drag) {
        if (!event.isTrusted || event.pointerId !== drag.pointerId) return;
        event.preventDefault();
        event.stopImmediatePropagation();
        drag.end = pagePoint(event);
        layout();
        return;
      }
      if (!active || modalSuspended || !event.isTrusted || drag || selectionMode === 'region' ||
          (open && selectionMode !== 'element')) return;
      const path = event.composedPath?.() || [];
      if (path.includes(host)) return;
      const target = path.find(node => node?.nodeType === 1);
      const next = target && connected(target) ? target : null;
      if (next === hovered) return;
      hovered = next;
      if (hovered) renderInfo(hovered);
      layout();
    };
    const pointerUp = event => {
      if (!drag || !event.isTrusted || event.pointerId !== drag.pointerId) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      const next = regionRect(drag.start, pagePoint(event));
      drag = null;
      if (next.width > 0 && next.height > 0) {
        if (!open) {
          targets.length = 0;
          draftChains.clear();
        }
        region = next;
        selected = null;
        selectedId = '';
        selectionMode = null;
        open = true;
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
    // The hover card follows the element under the pointer, so it has to die the
    // moment the pointer is no longer over the page. Pointer moves inside the
    // document already retarget the card, but leaving the webview (onto the app
    // chrome, another window, or off-screen) fires no further pointermove and
    // used to leave the last card pinned over the page.
    const dropHover = () => {
      if (!hovered) return;
      hovered = null;
      layout();
    };
    const pointerOut = event => {
      if (event.relatedTarget) return;
      dropHover();
    };
    const escape = event => {
      if (!active || modalSuspended || !event.isTrusted || event.key !== 'Escape') return;
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
    document.addEventListener('pointerdown', pointerDown, true);
    document.addEventListener('pointermove', pointerMove, true);
    document.addEventListener('pointerup', pointerUp, true);
    document.addEventListener('pointercancel', pointerCancel, true);
    document.addEventListener('mousedown', suppress, true);
    document.addEventListener('click', select, true);
    document.addEventListener('keydown', escape, true);
    document.addEventListener('pointerout', pointerOut, true);
    globalThis.addEventListener('blur', dropHover);
    globalThis.addEventListener('scroll', layout, true);
    globalThis.addEventListener('resize', layout);
    const removed = (target, chain, records) => !connected(target) || records.some(record =>
      Array.from(record.removedNodes || []).some(node => node === target || chain.includes(node) || node.contains?.(target)));
)ACEANNOTATION" R"ACEANNOTATION(
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
      document.removeEventListener('pointerout', pointerOut, true);
      globalThis.removeEventListener('blur', dropHover);
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
        case 'start': active = true; markersHidden = false; modalSuspended = false; hovered = null; break;
        case 'read': break;
        case 'set_modal_suspended':
          modalSuspended = next.enabled === true;
          if (modalSuspended) hovered = null;
          break;
        case 'exit':
          textPreview.cancel();
          styles.cancel();
          active = false;
          markersHidden = false;
          modalSuspended = false;
          clearDraft();
          break;
        case 'cancel': textPreview.cancel(); styles.cancel(); clearDraft(); break;
        case 'hide_markers':
          markersHidden = next.enabled === true;
          break;
        case 'discard':
          textPreview.discardAll();
          styles.discardAll();
          clearPins();
          clearDraft();
          original = false;
          markersHidden = false;
          modalSuspended = false;
          textPreview.setOriginal(false);
          styles.setOriginal(false);
          break;
        case 'remove_pins': {
          const released = new Set(Array.isArray(next.numbers) ? next.numbers : []);
          for (let index = pins.length - 1; index >= 0; index -= 1) {
            if (!released.has(pins[index].number)) continue;
            pins[index].marker.remove();
            pins[index].outline?.remove();
            pins.splice(index, 1);
          }
          break;
        }
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
          if (!active || selectionMode) return {error:'stale_target'};
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
        case 'edit_record': {
          // 从待发卡片点铅笔回到页面（#217）：按保存时记下的 locator 找回元素，
          // 重新进入批注态并把草稿切换成这一条。窗口内的重放（#216 的 replayable）
          // 以后可以复用这条路径，所以只做「定位 + 选中」，不碰描述。
          const locator = typeof next.locator === 'string' ? next.locator : '';
          if (!locator || locator.length > 2048 || selectionMode) return {error:'stale_target'};
          let element = null;
          try { element = document.querySelector(locator); } catch { element = null; }
          if (!connected(element)) return {error:'stale_target'};
          active = true;
          markersHidden = false;
          modalSuspended = false;
          targets.length = 0;
          targets.push(element);
          draftChains.set(element, ancestors(element));
          observe(element);
          region = null;
          selected = element;
          if (!ids.has(element)) ids.set(element, `target-${++sequence}`);
          selectedId = ids.get(element);
          styles.select(element, selectedId);
          open = true;
          layout();
          break;
        }
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
          marker.setAttribute('data-ace-annotation-pin', String(next.number));
          marker.style.cssText = 'position:fixed;pointer-events:none;border-radius:12px;padding:2px 6px;background:Highlight;color:HighlightText';
          shadow.append(marker);
          let savedOutline = null;
          if (region) {
            savedOutline = document.createElement('div');
            savedOutline.setAttribute('data-ace-annotation-pin-outline', String(next.number));
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
