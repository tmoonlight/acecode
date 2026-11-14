import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('shared image lightbox owns bounded wheel zoom and captured left drag', () => {
  const component = source('components/ImageLightbox.jsx');
  assert.match(component, /viewport\.addEventListener\('wheel', handleWheel, options\)/);
  assert.match(component, /const options = \{ passive: false \}/);
  assert.match(component, /if \(event\.button !== 0\) return/);
  assert.match(component, /setPointerCapture\?\.\(event\.pointerId\)/);
  assert.match(component, /onPointerMove=\{movePan\}/);
  assert.match(component, /onPointerCancel=\{stopPan\}/);
  assert.match(component, /aria-label="缩小预览"/);
  assert.match(component, /aria-label="放大预览"/);
  assert.match(component, /aria-label="适应窗口"/);
  assert.match(component, /!isDismissShortcut\(event\)/);
});

run('shared image lightbox retains caller metadata and backdrop boundaries', () => {
  const component = source('components/ImageLightbox.jsx');
  assert.match(component, /\.\.\.contextMenuAttrs/);
  assert.match(component, /width=\{previewWidth\}/);
  assert.match(component, /height=\{previewHeight\}/);
  assert.match(component, /backgroundColor: previewCanvasColor/);
  assert.match(component, /registerMermaidExportTarget\(element, mermaidExport\)/);
  assert.match(component, /data-mermaid-export-target=\{mermaidExport \? 'true' : undefined\}/);
  assert.match(component, /onClick=\{\(event\) => event\.stopPropagation\(\)\}/);
  assert.match(component, /onClick=\{\(\) => onClose\?\.\(\)\}/);
  assert.match(component, /draggable="false"/);
});

run('Mermaid preview host is mounted once and owns its Blob URL lifetime', () => {
  const main = source('main.jsx');
  const host = source('components/MermaidPreviewHost.jsx');
  assert.match(main, /<MermaidPreviewHost \/>/);
  assert.match(host, /window\.addEventListener\(MERMAID_PREVIEW_EVENT, openPreview\)/);
  assert.match(host, /new window\.Blob\(/);
  assert.match(host, /window\.URL\.createObjectURL/);
  assert.match(host, /window\.URL\.revokeObjectURL/);
  assert.match(host, /canvasColor: mermaidPreviewCanvasColor\(detail\.theme\)/);
  assert.match(host, /mermaidExport: detail/);
  assert.match(host, /return <ImageLightbox preview=\{preview\} onClose=\{closePreview\} \/>/);
});

run('ready Mermaid images are native preview buttons backed by sanitized SVG detail', () => {
  const renderer = source('lib/mermaidRenderer.js');
  const styles = source('styles/globals.css');
  assert.match(renderer, /owner\.createElement\('button'\)/);
  assert.match(renderer, /previewTrigger\.type = 'button'/);
  assert.match(renderer, /dispatchMermaidPreview\(win, \{/);
  assert.match(renderer, /registerMermaidExportTarget\(previewTrigger, \{/);
  assert.match(renderer, /source,/);
  assert.match(renderer, /svg: result\.svg/);
  assert.match(renderer, /theme,/);
  assert.match(renderer, /target\.replaceChildren\(previewTrigger\)/);
  assert.match(styles, /\.ace-mermaid-preview-trigger/);
  assert.match(styles, /\.ace-mermaid-preview-trigger\s*\{[^}]*width: max-content/);
  assert.match(styles, /cursor: zoom-in/);
});

// 场景:模型画了比对话栏宽的 Mermaid 图(多分组的流程图、十几个参与者的时序图)。
// 期望:行内只缩不放 —— 窄图保持原始尺寸,宽图等比缩到栏宽;高度不写死,跟着宽度按比例走。
// 回归:图片按原始宽高硬写(max-width: none + 内联 height),宽图超出对话栏,只能横向拖动滚动条。
run('wide Mermaid diagrams shrink to the chat column instead of scrolling sideways', () => {
  const renderer = source('lib/mermaidRenderer.js');
  const styles = source('styles/globals.css');
  assert.match(renderer, /image\.style\.width = `\$\{result\.width\}px`/);
  assert.doesNotMatch(renderer, /image\.style\.height\s*=/);
  assert.match(styles, /\.ace-mermaid-preview-trigger\s*\{[^}]*max-width: 100%/);
  assert.match(styles, /\.ace-mermaid-svg\s*\{[^}]*height: auto;[^}]*max-width: 100%/);
});

run('shared context menu intercepts ordinary browser right clicks and gates native-only actions', () => {
  const component = source('components/DesktopContextMenu.jsx');
  assert.match(component, /const candidateTargets = contextTargetsFromElement\(rawTarget\)/);
  assert.match(component, /rawTarget\.closest\('\.ace-console-term'\)\) return/);
  assert.match(component, /const allowNativeActions = useMemo\(\(\) => isDesktopShell\(\) \|\| isWebappCompat\(\), \[\]\)/);
  assert.match(component, /if \(!canRunContextMenuAction\(action, \{ allowNativeActions \}\)\) return/);
  assert.match(component, /buildDesktopContextMenuItems\(\{[\s\S]*allowNativeActions,/);
  assert.doesNotMatch(component, /shouldUseCustomContextMenu/);
  assert.match(component, /exportMermaidAsset\(target, format\)/);
  assert.doesNotMatch(component, /if \(!desktop\) return undefined/);
  assert.match(component, /if \(!menu && !pendingConfirm\) return null/);
});
