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
    document.documentElement.append(host);
    const connected = target => target?.isConnected && target.ownerDocument === rootDocument;
    const rect = target => {
      const bounds = target.getBoundingClientRect();
      return {x:bounds.x, y:bounds.y, width:bounds.width, height:bounds.height};
    };
    const layout = () => {
      const visible = active && connected(selected);
      outline.style.display = visible ? 'block' : 'none';
      control.style.display = visible && !open ? 'block' : 'none';
      if (visible) {
        const bounds = rect(selected);
        Object.assign(outline.style, {left:`${bounds.x}px`, top:`${bounds.y}px`, width:`${bounds.width}px`, height:`${bounds.height}px`});
        control.style.left = `${Math.max(0, Math.min(innerWidth - control.offsetWidth, bounds.x))}px`;
        control.style.top = `${Math.max(0, Math.min(innerHeight - control.offsetHeight, bounds.y + bounds.height))}px`;
      }
      for (const pin of pins) {
        pin.marker.style.display = connected(pin.target) ? 'block' : 'none';
        if (!connected(pin.target)) continue;
        const bounds = rect(pin.target);
        pin.marker.style.left = `${bounds.x}px`;
        pin.marker.style.top = `${bounds.y}px`;
      }
    };
    const snapshot = () => {
      layout();
      const target = connected(selected) ? {
        id:selectedId,
        tag:selected.localName.slice(0, 64),
        text:(selected.innerText || selected.textContent || '').slice(0, 512),
        locator:selected.localName + (selected.id ? '#' + selected.id.slice(0, 128) : ''),
        bounds:rect(selected),
        text_preview:textPreview.describe(selected, selectedId),
      } : null;
      return {document_id:documentId, active, popup:!!target && open, target, original,
        viewport:{width:innerWidth, height:innerHeight}};
    };
    const select = event => {
      if (!active || !event.isTrusted || event.composedPath().includes(host)) return;
      if (event.button !== 0) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      if (open) return;
      const target = event.composedPath().find(node => node?.nodeType === 1);
      if (!connected(target)) return;
      selected = target;
      if (!ids.has(target)) ids.set(target, `target-${++sequence}`);
      selectedId = ids.get(target);
      open = false;
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
      active = false;
      textPreview.cancel();
      selected = null;
      open = false;
      layout();
    };
    control.addEventListener('click', event => {
      if (!active || !event.isTrusted || !connected(selected)) return;
      event.preventDefault();
      event.stopPropagation();
      open = true;
      layout();
    });
    document.addEventListener('pointerdown', suppress, true);
    document.addEventListener('mousedown', suppress, true);
    document.addEventListener('click', select, true);
    document.addEventListener('keydown', escape, true);
    globalThis.addEventListener('scroll', layout, true);
    globalThis.addEventListener('resize', layout);
    runtime = Object.freeze({document_id:documentId, dispose() {
      textPreview.dispose();
      document.removeEventListener('pointerdown', suppress, true);
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
        case 'exit': textPreview.cancel(); active = false; selected = null; open = false; break;
        case 'cancel': textPreview.cancel(); selected = null; open = false; break;
        case 'original': original = next.enabled; textPreview.setOriginal(original); break;
        case 'preview_text': {
          if (!active || !open || !connected(selected) || selectedId !== next.target_id) return {error:'stale_target'};
          const result = textPreview.preview(selected, selectedId, next.text);
          if (result.error) return result;
          break;
        }
        case 'save': {
          if (!active || !open || !connected(selected) || selectedId !== next.target_id) return {error:'stale_target'};
          const result = textPreview.save();
          if (result.error) return result;
          const marker = document.createElement('span');
          marker.textContent = String(next.number);
          marker.style.cssText = 'position:fixed;pointer-events:none;border-radius:12px;padding:2px 6px;background:Highlight;color:HighlightText';
          shadow.append(marker);
          pins.push({target:selected, marker});
          selected = null;
          open = false;
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
