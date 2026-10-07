// Runs only inside the sandboxed invitation iframe, never in the live office.
function previewRuntime(labels) {
  let paused = matchMedia('(prefers-reduced-motion: reduce)').matches;
  let pendingFrame = null;
  let frameId = null;
  let timer = null;
  let step = 0;
  const nativeFrame = window.requestAnimationFrame.bind(window);
  window.requestAnimationFrame = callback => {
    pendingFrame = callback;
    if (paused || document.hidden) return 0;
    frameId = nativeFrame(time => { frameId = null; pendingFrame = null; callback(time); });
    return frameId;
  };
  const actor = (id, name, extra = {}) => ({ id, name, root: id === 'demo-root',
    state: 'work', label: labels.working, busy: true, seed: id.length * 137,
    contextKnown: true, contextRatio: 0.35, contextTokens: 3500, transfers: [], ...extra });
  const draw = () => {
    if (!window.AgentOffice) return;
    const root = actor('demo-root', labels.lead, { path: '/root' });
    const worker = actor('demo-worker', labels.coder, { path: '/root/code', parentPath: '/root',
      state: step % 2 ? 'think' : 'work', label: step % 2 ? labels.thinking : labels.working,
      transfers: step ? [{ seq: step, sender_session_id: 'demo-root', recipient: '/root/code', type: 'NEW_TASK' }] : [] });
    window.AgentOffice.applySnapshot({ version: 1, follow: true, connected: true, complete: true,
      seed: 137, selected: { sessionId: 'demo-root', workspaceHash: 'preview', title: labels.title },
      offices: [], agents: [root, worker, actor('demo-reviewer', labels.reviewer, { state: 'think', label: labels.thinking })], overflow: 0 });
    step++;
  };
  const resume = () => {
    if (paused || document.hidden) return;
    if (pendingFrame && frameId === null) window.requestAnimationFrame(pendingFrame);
    if (timer === null) timer = setInterval(draw, 5000);
  };
  const stop = () => {
    if (frameId !== null) cancelAnimationFrame(frameId);
    frameId = null;
    clearInterval(timer); timer = null;
  };
  window.addEventListener('message', event => {
    if (event.source !== parent || event.data?.type !== 'ace-office-preview-pause') return;
    paused = !!event.data.paused;
    stop(); resume();
  });
  document.addEventListener('visibilitychange', () => { stop(); resume(); });
  window.addEventListener('DOMContentLoaded', () => { draw(); resume(); });
}

export function buildOfficePreviewDocument(html) {
  const labels = { title: '虚拟办公室预览', lead: '协调工作', coder: '编写代码', reviewer: '检查改动',
    working: '工作中', thinking: '思考中' };
  const script = `(${previewRuntime.toString()})(${JSON.stringify(labels).replaceAll('<', '\\u003c')});`;
  return html.replace('<head>', `<head><script>${script}</script>`)
    .replace('</head>', '<style>.office-controls,.office-notice,.office-members,.resize-handle{display:none!important}body{pointer-events:none}</style></head>');
}
