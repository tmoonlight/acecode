// Shared companion-pet bubbles: renderer, icons, typography and colors.
(() => {
  const style = document.createElement('style');
  style.textContent = ":root{--pet-font:-apple-system,BlinkMacSystemFont,\"PingFang SC\",\"Segoe UI\",sans-serif;--pet-font-size:14px;--pet-ink:#27324f;--card:#fff;--card-ghost:#f2f2f2;--card-ghost-2:#e6e6e6;--card-text:#1d1d1f;--card-muted:#555b66;\n  --card-shadow:0 4px 14px rgba(15,20,30,.16),0 1px 3px rgba(15,20,30,.10);--card-button:#e9e9eb;--card-button-hover:#dcdce0;\n  --ok:#16a34a;--warn:#d97706;--danger:#dc2626;--idle:#8a8f98}\n@media(prefers-color-scheme:dark){:root{--card:#2b2d31;--card-ghost:#25272b;--card-ghost-2:#1f2124;--card-text:#f2f2f3;--card-muted:#b4b8c0;\n  --card-shadow:0 4px 14px rgba(0,0,0,.45),0 1px 3px rgba(0,0,0,.3);--card-button:#3a3d42;--card-button-hover:#46494f}}\n.pet-activity-cards{font:calc(var(--pet-font-size) * var(--stack-fit,1))/1.6 var(--pet-font)}\n.card{position:relative;display:flex;align-items:center;gap:.6em;min-height:3.3em;padding:.5em .9em .5em 1em;border-radius:1.6em;\n  background:var(--card);color:var(--card-text);box-shadow:var(--card-shadow);cursor:pointer}\n.card:focus-visible{outline:2px solid #4378be;outline-offset:2px}\n.text{flex:1;min-width:0}\n.head{display:flex;align-items:center;gap:.4em;font-weight:700}\n.name{min-width:0;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}\n/* A block box: on an inline span the ellipsis does nothing and long text runs under the stop button. */\n.body{display:block;margin-top:.1em;color:var(--card-muted);overflow:hidden;white-space:nowrap;text-overflow:ellipsis}\n.icon{flex:none;width:1.1em;height:1.1em;display:grid;place-items:center}\n.icon svg{width:100%;height:100%}\n.spinner{width:.9em;height:.9em;border:2px solid var(--card-button);border-top-color:var(--idle);border-radius:50%;animation:spin .9s linear infinite}\n@keyframes spin{to{transform:rotate(360deg)}}\n.stop{flex:none;width:2.2em;height:2.2em;border:0;border-radius:50%;background:var(--card-button);display:grid;place-items:center;cursor:pointer;padding:0}\n.stop:hover{background:var(--card-button-hover)}\n.stop i{width:.75em;height:.75em;border-radius:2px;background:var(--idle)}\n.dismiss{position:absolute;left:-.2em;top:-.2em;width:1.4em;height:1.4em;border:0;border-radius:50%;background:var(--card);color:var(--card-text);\n  box-shadow:0 1px 3px rgba(0,0,0,.25);display:none;place-items:center;cursor:pointer;padding:0;font:inherit;font-weight:700;line-height:1}\n.card:hover .dismiss,.card:focus-within .dismiss{display:grid}\n.stack.collapsed.layers .card .dismiss{display:none}\n";
  document.head.append(style);
  const ICONS = {
    review: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--ok)"/><path d="m4.6 8.3 2.2 2.2 4.6-4.8" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>',
    failed: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--danger)"/><path d="M8 4.2v4.6M8 11.3v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
    waiting: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--warn)"/><path d="M8 4.2v4.6M8 11.3v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
    stopped: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--idle)"/><rect x="5.3" y="5.3" width="5.4" height="5.4" rx="1" fill="#fff"/></svg>',
    info: '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="8" fill="var(--idle)"/><path d="M8 7.2v4.4M8 4.6v.1" stroke="#fff" stroke-width="1.9" stroke-linecap="round"/></svg>',
  };
  const STATUS_TEXT = { running: '运行中', waiting: '需要你处理', review: '已完成', failed: '失败', stopped: '已停止' };
  const RESULT = ['review', 'failed', 'stopped'];

  function create(initial, handlers = {}) {
    let card = initial;
    const el = document.createElement('div');
    el.className = 'card'; el.tabIndex = 0; el.setAttribute('role', 'listitem');
    el.innerHTML = '<span class="icon"></span><span class="text"><span class="head"><span class="name"></span></span><span class="body"></span></span>';
    const icon = el.querySelector('.icon'), name = el.querySelector('.name'), body = el.querySelector('.body');
    const stop = document.createElement('button');
    stop.type = 'button'; stop.className = 'stop'; stop.title = '停止这个会话'; stop.innerHTML = '<i></i>';
    stop.addEventListener('click', event => { event.stopPropagation(); handlers.stop?.(card); });
    const close = document.createElement('button');
    close.type = 'button'; close.className = 'dismiss'; close.textContent = '×';
    close.addEventListener('click', event => { event.stopPropagation(); handlers.dismiss?.(card); });
    el.append(stop, close);
    el.addEventListener('click', () => handlers.open?.(card));
    el.addEventListener('keydown', event => {
      if (event.target !== el) return;
      if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); handlers.open?.(card); }
      else if (event.key === 'Escape') handlers.collapse?.();
    });
    const update = next => {
      const statusChanged = el.dataset.status !== next.status;
      card = next; el.dataset.key = card.key || card.sessionId || ''; el.dataset.status = card.status;
      const label = STATUS_TEXT[card.status];
      el.setAttribute('aria-label', [card.title, label, card.body].filter(Boolean).join('，'));
      el.title = card.local ? '' : `${card.title}\n${card.body}\n点击打开这个会话`;
      if (statusChanged) icon.innerHTML = card.status === 'running' ? '<i class="spinner"></i>' : ICONS[card.status] || '';
      name.textContent = card.title; body.textContent = card.body || label || '';
      stop.hidden = card.status !== 'running';
      stop.setAttribute('aria-label', `停止「${card.title}」`);
      close.hidden = !handlers.dismiss;
      close.title = RESULT.includes(card.status) ? '标为已读并移除' : '暂时隐藏';
      close.setAttribute('aria-label', `移除「${card.title}」`);
    };
    update(card);
    return { element: el, update };
  }
  function applyTranscriptFont(font) {
    const root = document.documentElement.style;
    const size = String(font?.size || '').trim(), family = String(font?.family || '').trim();
    const before = root.cssText;
    if (/^\d+(\.\d+)?px$/.test(size)) root.setProperty('--pet-font-size', size);
    if (family && !/[;{}<>]/.test(family)) root.setProperty('--pet-font', family);
    return root.cssText !== before;
  }
  function placePopup(anchor, desired, viewport, gap = 10, margin = 8) {
    const w = Math.max(1, viewport.width), h = Math.max(1, viewport.height);
    margin = Math.max(0, Math.min(margin, w / 4, h / 4));
    const width = Math.max(1, Math.min(desired.width, w - 2 * margin));
    const below = Math.max(0, h - margin - anchor.bottom - gap);
    const above = Math.max(0, anchor.top - gap - margin);
    const upward = desired.height > below && above > below;
    const height = Math.max(1, Math.min(desired.height, upward ? above : below));
    const clamp = (value, min, max) => Math.max(min, Math.min(value, Math.max(min, max)));
    return { left: clamp((anchor.left + anchor.right - width) / 2, margin, w - margin - width),
      top: clamp(upward ? anchor.top - gap - height : anchor.bottom + gap, margin, h - margin - height),
      width, height, upward, clamped: height < desired.height || width < desired.width };
  }
  window.PetActivityCards = Object.freeze({ create, applyTranscriptFont, placePopup, results: RESULT });
})();
