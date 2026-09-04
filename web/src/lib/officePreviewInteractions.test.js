import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

function source(relativePath) {
  return readFileSync(new URL(relativePath, import.meta.url), 'utf8');
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

run('Office previews share icon-only bounded zoom controls and native Ctrl-wheel capture', () => {
  const controls = source('../components/OfficePreviewControls.jsx');
  const styles = source('../styles/globals.css');

  assert.equal((controls.match(/className="ace-office-preview-zoom-button"/g) || []).length, 2);
  assert.match(controls, /title="缩小"[\s\S]*aria-label="缩小"/);
  assert.match(controls, /title="放大"[\s\S]*aria-label="放大"/);
  assert.match(controls, /if \(!event\.ctrlKey\) return;/);
  assert.match(controls, /addEventListener\('wheel', onWheel, \{ passive: false, capture: true \}\)/);
  assert.doesNotMatch(controls, /Math\.round\(zoom \* 100\)|\{zoom\s*\*\s*100\}|100%/);
  assert.match(styles, /\.ace-office-preview-controls\s*\{[\s\S]*top:\s*8px;[\s\S]*right:\s*8px;/);
  assert.match(styles, /\.ace-office-preview-shell\s*\{[\s\S]*min-width:\s*0;[\s\S]*width:\s*100%;/);
});

run('Word preview keeps browser zoom centered on the visible viewport', () => {
  const preview = source('../components/FilePreviewContent.jsx');
  const styles = source('../styles/globals.css');
  const wordPreview = preview.slice(
    preview.indexOf('function WordPreview'),
    preview.indexOf('function SpreadsheetPreview'),
  );
  assert.match(preview, /function WordPreview[\s\S]*useOfficePreviewZoom\(path, shellRef\)/);
  assert.match(wordPreview, /className="ace-side-docx-renderer"[\s\S]*style=\{\{[\s\S]*zoom,[\s\S]*\}\}/);
  assert.doesNotMatch(wordPreview, /100\s*\/\s*zoom/);
  assert.match(styles, /\.ace-side-docx-renderer\s*\{[\s\S]*width:\s*100%;/);
  assert.match(wordPreview, /ref=\{viewportRef\}[\s\S]*className="ace-side-docx-preview"/);
  assert.match(wordPreview, /renderAsync\([\s\S]*?\)\.then\(\(\) => \{/);
  assert.match(wordPreview, /if \(cancelled \|\| fitApplied\) return;[\s\S]*new ResizeObserver\(scheduleFit\)/);
  assert.match(wordPreview, /fitApplied = true;[\s\S]*fitObserver\?\.disconnect\(\)/);
  assert.match(wordPreview, /applyInitialWordPreviewFit\(viewportRef\.current, host, setZoom\)/);
  assert.match(wordPreview, /<OfficePreviewControls[\s\S]*onZoomIn=\{zoomIn\}[\s\S]*onZoomOut=\{zoomOut\}/);
});

run('Spreadsheet resize and zoom reload canvas plus native scrollbar geometry', () => {
  const preview = source('../components/FilePreviewContent.jsx');
  const spreadsheetBlock = preview.slice(preview.indexOf('function SpreadsheetPreview'));
  assert.match(spreadsheetBlock, /officePreviewLogicalSize\(host\.clientWidth, zoomRef\.current, 120\)/);
  assert.match(spreadsheetBlock, /officePreviewLogicalSize\(host\.clientHeight, zoomRef\.current, 120\)/);
  assert.match(spreadsheetBlock, /root\.style\.zoom = String\(zoomRef\.current\)/);
  assert.match(spreadsheetBlock, /spreadsheet\.sheet\?\.reload\?\.\(\)/);
  assert.match(spreadsheetBlock, /new ResizeObserver\(syncSpreadsheetGeometry\)/);
  assert.doesNotMatch(spreadsheetBlock, /reRender/);
  assert.doesNotMatch(spreadsheetBlock, /overflow-x|scrollLeft/);
});
