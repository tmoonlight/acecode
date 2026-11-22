#pragma once

namespace acecode::desktop {

inline constexpr const char kPageAnnotationStyleRuntime[] = R"ACESTYLES(
(rootDocument => {
  const sides = ['top', 'right', 'bottom', 'left'];
  const groups = {
    'border-radius':['border-top-left-radius','border-top-right-radius','border-bottom-right-radius','border-bottom-left-radius'],
    'border-color':sides.map(side => `border-${side}-color`),
    'border-width':sides.map(side => `border-${side}-width`),
    padding:sides.map(side => `padding-${side}`),
    margin:sides.map(side => `margin-${side}`),
    gap:['row-gap','column-gap'],
  };
  const fields = ['color','background-color','opacity','font-family','font-size','font-weight',
    'width','height','letter-spacing','word-spacing','row-gap','column-gap',
    ...Object.keys(groups),...Object.values(groups).flat()];
  const properties = [...new Set(fields.flatMap(field => groups[field] || [field])),
    ...sides.map(side => `border-${side}-style`)];
  const token = `data-ace-preview-${Array.from(crypto.getRandomValues(new Uint32Array(4)),value => value.toString(16)).join('')}`;
  const targets = new Map();
  const sheets = new Map();
  const observedRoots = new Set();
  let original = false;
  let nextToken = 0;
  let rendered = new Set();
  const computed = target => {
    const style = getComputedStyle(target);
    return Object.fromEntries([...new Set([...fields,...properties])].map(field => [field,style.getPropertyValue(field)]));
  };
  const connected = target => target.isConnected && target.ownerDocument === rootDocument;
  const apply = () => {
    const rules = new Map();
    const settling = new Map();
    const nextRendered = new Set();
    for (const state of targets.values()) {
      if (!connected(state.target)) continue;
      const root = state.target.getRootNode();
      if (!observedRoots.has(root)) {
        observedRoots.add(root);
        observer.observe(root,{childList:true,subtree:true,attributes:true,attributeFilter:[token]});
      }
      const declarations = original ? {} : {...state.saved,...state.draft?.styles};
      const selector = Array(4).fill(`[${token}="${state.token}"]:not(#${token})`).join('');
      if (!Object.keys(declarations).length) {
        if (rendered.has(state)) settling.set(root,(settling.get(root) || '') + `${selector}{transition:none !important;}`);
        continue;
      }
      nextRendered.add(state);
      if (state.target.getAttribute(token) !== state.token) state.target.setAttribute(token,state.token);
      const rule = `${selector}{transition:none !important;animation:none !important;${Object.entries(declarations).map(([field,value]) => `${field}:${value} !important;`).join('')}}`;
      rules.set(root,(rules.get(root) || '') + rule);
    }
    for (const root of [...rules.keys(),...settling.keys()]) {
      if (!sheets.has(root)) {
        const sheet = new CSSStyleSheet();
        sheets.set(root,sheet);
      }
    }
    for (const [root,sheet] of sheets) {
      if (!root.adoptedStyleSheets.includes(sheet)) root.adoptedStyleSheets = [...root.adoptedStyleSheets,sheet];
      sheet.replaceSync((rules.get(root) || '') + (settling.get(root) || ''));
    }
    if (settling.size) {
      for (const state of rendered) if (connected(state.target)) getComputedStyle(state.target).getPropertyValue('color');
      for (const root of settling.keys()) sheets.get(root).replaceSync(rules.get(root) || '');
    }
    rendered = nextRendered;
  };
  const observer = new MutationObserver(() => {
    for (const root of observedRoots) {
      for (const candidate of root.querySelectorAll(`[${token}]`)) {
        if (![...targets.values()].some(state => state.target === candidate && candidate.getAttribute(token) === state.token)) {
          candidate.removeAttribute(token);
        }
      }
    }
    apply();
  });
  const stateFor = (target,id) => {
    let state = targets.get(id);
    if (state && state.target !== target) throw new Error('stale_target');
    if (!state) {
      state = {target,id,token:String(++nextToken),saved:{},draft:null};
      targets.set(id,state);
      const root = target.getRootNode();
      if (!observedRoots.has(root)) {
        observedRoots.add(root);
        observer.observe(root,{childList:true,subtree:true,attributes:true,attributeFilter:[token]});
      }
    }
    return state;
  };
  const changes = state => {
    if (!state.draft) return [];
    if (original) return state.draft.comparison || [];
    const values = computed(state.target);
    return [...state.draft.touched].filter(field => values[field] !== state.draft.baseline[field])
      .map(property => ({target_id:state.id,property,old_value:state.draft.baseline[property],new_value:values[property]}));
  };
  const select = (target,id) => {
    const state = stateFor(target,id);
    if (!state.draft) {
      const comparing = original;
      if (comparing) { original = false; apply(); }
      state.draft = {styles:{},baseline:computed(target),touched:new Set(),ratio:null,errors:[]};
      if (comparing) { original = true; apply(); }
    }
    return state;
  };
  return {
    select,
    preview(target,id,styles,ratioLock) {
      if (original) throw new Error('original_view_active');
      const state = select(target,id);
      const draft = state.draft;
      for (const [field,value] of Object.entries(styles)) {
        if (!fields.includes(field) || typeof value !== 'string' || value.length > 256 ||
            /[;{}@!<>\\\u0000-\u001f]/.test(value) || /(?:url|var|env|attr|expression)\s*\(/i.test(value) ||
            (value && !CSS.supports(field,value))) throw new Error('invalid_style_value');
      }
      const previous = {styles:{...draft.styles},touched:new Set(draft.touched),ratio:draft.ratio};
      if (ratioLock !== undefined) {
        const values = computed(target);
        const width = parseFloat(values.width);
        const height = parseFloat(values.height);
        if (ratioLock && (!(width > 0) || !(height > 0))) throw new Error('invalid_current_ratio');
        draft.ratio = ratioLock ? width / height : null;
      }
      for (const [field,value] of Object.entries(styles)) {
        const expanded = groups[field] || [field];
        const declaration = rootDocument.createElement('span').style;
        if (value) declaration.setProperty(field,value);
        for (const property of expanded) {
          draft.touched.add(property);
          if (value) draft.styles[property] = declaration.getPropertyValue(property);
          else delete draft.styles[property];
        }
        if (field === 'border-width' && value) {
          const values = computed(target);
          for (const side of sides) {
            const property = `border-${side}-style`;
            if (values[property] === 'none') {
              draft.styles[property] = 'solid';
              draft.touched.add(property);
            }
          }
        }
        if (field === 'border-width' && !value) {
          for (const side of sides) delete draft.styles[`border-${side}-style`];
        }
      }
      apply();
      if (draft.ratio && (styles.width || styles.height)) {
        const primary = styles.width ? 'width' : 'height';
        const linked = primary === 'width' ? 'height' : 'width';
        const value = parseFloat(computed(target)[primary]);
        if (!Number.isFinite(value) || value < 0) {
          Object.assign(draft,previous);
          apply();
          throw new Error('invalid_current_ratio');
        }
        draft.styles[linked] = `${primary === 'width' ? value / draft.ratio : value * draft.ratio}px`;
        draft.touched.add(linked);
        apply();
      }
      draft.errors = [];
    },
    describe(target,id) {
      const state = stateFor(target,id);
      const values = computed(target);
      const baseline = state.draft?.baseline;
      const previous = baseline
        ? Object.fromEntries([...new Set([...fields,...properties])].map(field => [field,baseline[field] ?? values[field]]))
        : values;
      return {values,previous_values:previous,styles:{...state.draft?.styles},
        ratio_locked:!!state.draft?.ratio,ratio:state.draft?.ratio || null,errors:state.draft?.errors || []};
    },
    differences() { return [...targets.values()].flatMap(changes); },
    save() {
      const differences = this.differences();
      for (const state of targets.values()) {
        const changed = new Set(changes(state).map(change => change.property));
        for (const [field,value] of Object.entries(state.draft?.styles || {})) {
          if (changed.has(field)) state.saved[field] = value;
        }
        state.draft = null;
      }
      apply();
      return differences;
    },
    cancel() { for (const state of targets.values()) state.draft = null; apply(); },
    discardAll() {
      for (const state of targets.values()) {
        state.saved = {};
        state.draft = null;
      }
      apply();
    },
    invalidate(ids) {
      for (const id of ids) {
        const state = targets.get(id);
        if (!state) continue;
        state.saved = {};
        state.draft = null;
      }
      apply();
    },
    setOriginal(enabled) {
      if (enabled && !original) {
        for (const state of targets.values()) if (state.draft) state.draft.comparison = changes(state);
      }
      original = enabled;
      apply();
    },
    dispose() {
      original = true;
      apply();
      observer.disconnect();
      for (const [root,sheet] of sheets) root.adoptedStyleSheets = root.adoptedStyleSheets.filter(item => item !== sheet);
      for (const state of targets.values()) {
        if (state.target.getAttribute(token) === state.token) state.target.removeAttribute(token);
      }
      targets.clear();
      sheets.clear();
      observedRoots.clear();
      rendered.clear();
    },
  };
})
)ACESTYLES";

} // namespace acecode::desktop
