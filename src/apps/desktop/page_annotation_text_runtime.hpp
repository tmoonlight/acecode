#pragma once

namespace acecode::desktop {

inline constexpr const char kPageAnnotationTextRuntime[] = R"ACETEXT(
(() => {
  const originals = new Map();
  const entries = new Map();
  const saved = [];
  const drafts = new Map();
  let original = false;
  const collect = target => {
    const nodes = [];
    const visit = element => {
      if (['script', 'style', 'noscript', 'template', 'input', 'textarea', 'select'].includes(element.localName)) return;
      const style = getComputedStyle(element);
      if (style.display === 'none' || style.visibility === 'hidden' || style.visibility === 'collapse') return;
      for (const child of element.childNodes) {
        if (child.nodeType === 3) nodes.push(child);
        else if (child.nodeType === 1) visit(child);
      }
    };
    visit(target);
    return nodes;
  };
  const valid = entry => {
    const current = collect(entry.target);
    const expected = entry.nodes.filter(node => node !== entry.inserted || node.parentNode);
    return entry.target.isConnected && current.length === expected.length &&
      current.every((node, index) => node === expected[index]) && entry.nodes.every((node, index) =>
        node === entry.inserted && !node.parentNode || node.parentNode === entry.parents[index] && entry.target.contains(node));
  };
  const render = () => {
    for (const entry of entries.values()) {
      if (entry.inserted && !entry.inserted.parentNode && entry.target.isConnected && !original &&
          (saved.some(layer => layer.entry === entry) || drafts.has(entry.id))) entry.target.append(entry.inserted);
    }
    for (const [node, value] of originals) node.data = value;
    if (!original) {
      for (const layer of [...saved, ...drafts.values()]) {
        if (!valid(layer.entry)) continue;
        layer.entry.nodes.forEach((node, index) => { node.data = layer.values[index]; });
      }
    }
    for (const entry of entries.values()) {
      if (entry.inserted && (original || !saved.some(layer => layer.entry === entry) && !drafts.has(entry.id))) {
        entry.inserted.remove();
      } else if (entry.inserted && !entry.inserted.parentNode && entry.target.isConnected) {
        entry.target.append(entry.inserted);
      }
    }
  };
  const ensure = (target, id) => {
    if (entries.has(id)) return entries.get(id);
    const nodes = collect(target);
    const value = nodes.map(node => node.data).join('');
    const style = getComputedStyle(target);
    const entry = {target, id, nodes, parents:nodes.map(node => node.parentNode), inserted:null,
      editable:value.length <= 4096 && style.display !== 'none' && style.visibility !== 'hidden' && style.visibility !== 'collapse' &&
        !['script', 'style', 'noscript', 'template', 'input', 'textarea', 'select', 'img', 'br', 'hr', 'canvas', 'video', 'audio', 'iframe'].includes(target.localName)};
    for (const node of nodes) if (!originals.has(node)) originals.set(node, node.data);
    entries.set(id, entry);
    return entry;
  };
  const effectiveNodes = (entry, includeDraft = true) => {
    const values = new Map(originals);
    for (const layer of [...saved, ...(includeDraft ? drafts.values() : [])]) {
      layer.entry.nodes.forEach((node, index) => values.set(node, layer.values[index]));
    }
    return entry.nodes.map(node => values.get(node) ?? '');
  };
  const effective = (entry, includeDraft = true) => effectiveNodes(entry, includeDraft).join('');
  const releaseUnused = () => {
    const retained = new Set(saved.map(layer => layer.entry));
    for (const [id, entry] of entries) if (!retained.has(entry)) entries.delete(id);
    const nodes = new Set([...retained].flatMap(entry => entry.nodes));
    for (const node of originals.keys()) if (!nodes.has(node)) originals.delete(node);
  };
  return Object.freeze({
    describe(target, id) {
      const entry = ensure(target, id);
      const current = valid(entry);
      return {value:entry.editable ? effective(entry) : '', editable:entry.editable && current,
        error:!current ? 'Text nodes changed; cancel and select the element again' :
          entry.editable ? '' : 'Text exceeds the editable limit or is not visible content'};
    },
    preview(target, id, value) {
      const entry = ensure(target, id);
      if (!entry.editable || typeof value !== 'string' || value.length > 4096) return {error:'invalid_text'};
      if (!valid(entry)) return {error:'stale_text_nodes'};
      const baseline = effective(entry, false);
      if (value === baseline) {
        drafts.delete(id);
        render();
        return {};
      }
      if (!entry.nodes.length) {
        entry.inserted = target.ownerDocument.createTextNode('');
        target.append(entry.inserted);
        entry.nodes.push(entry.inserted);
        entry.parents.push(target);
        originals.set(entry.inserted, '');
      } else if (entry.inserted && !entry.inserted.parentNode) target.append(entry.inserted);
      const previous = Array.from(baseline);
      const characters = Array.from(value);
      let start = 0;
      while (start < previous.length && start < characters.length && previous[start] === characters[start]) ++start;
      let suffix = 0;
      while (suffix < previous.length - start && suffix < characters.length - start &&
          previous[previous.length - 1 - suffix] === characters[characters.length - 1 - suffix]) ++suffix;
      const end = previous.length - suffix;
      const replacement = characters.slice(start, characters.length - suffix).join('');
      let offset = 0;
      let inserted = false;
      const values = effectiveNodes(entry, false).map(part => {
        const points = Array.from(part);
        const stop = offset + points.length;
        let next = points.slice(0, Math.max(0, start - offset)).join('');
        if (!inserted && start >= offset && start <= stop) { next += replacement; inserted = true; }
        next += points.slice(Math.max(0, end - offset)).join('');
        offset = stop;
        return next;
      });
      drafts.set(id, {entry, values, old_value:baseline, new_value:value});
      render();
      return {};
    },
    differences() {
      return [...drafts.values()].map(layer => ({target_id:layer.entry.id, property:'text',
        old_value:layer.old_value, new_value:layer.new_value}));
    },
    save() {
      if ([...drafts.values()].some(layer => !valid(layer.entry))) return {error:'stale_text_nodes'};
      const differences = this.differences();
      saved.push(...drafts.values());
      drafts.clear();
      render();
      releaseUnused();
      return {differences};
    },
    cancel() { drafts.clear(); render(); releaseUnused(); },
    setOriginal(enabled) { original = enabled; render(); },
    dispose() { saved.length = 0; drafts.clear(); original = true; render(); entries.clear(); originals.clear(); },
  });
})
)ACETEXT";

} // namespace acecode::desktop
