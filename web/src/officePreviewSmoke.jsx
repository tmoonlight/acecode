import React, { useEffect, useMemo, useState } from 'react';
import { createRoot } from 'react-dom/client';
import * as XLSX from 'xlsx';
import { FilePreviewContent } from './components/FilePreviewContent.jsx';
import './styles/globals.css';

function spreadsheetBlob() {
  const rows = Array.from({ length: 45 }, (_, rowIndex) => (
    Array.from({ length: 28 }, (_, columnIndex) => (
      rowIndex === 0
        ? `Column ${columnIndex + 1}`
        : `R${rowIndex + 1} C${columnIndex + 1} value`
    ))
  ));
  const workbook = XLSX.utils.book_new();
  const worksheet = XLSX.utils.aoa_to_sheet(rows);
  worksheet['!cols'] = Array.from({ length: 28 }, () => ({ wpx: 118 }));
  XLSX.utils.book_append_sheet(workbook, worksheet, 'Wide sheet');
  const bytes = XLSX.write(workbook, { type: 'array', bookType: 'xlsx' });
  return new Blob([bytes], {
    type: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  });
}

const xlsxBlob = spreadsheetBlob();
const pptxBlobPromise = fetch('/__office-preview-smoke.pptx').then((response) => {
  if (!response.ok) throw new Error(`PPTX fixture HTTP ${response.status}`);
  return response.blob();
});

const api = {
  async readFileBlob(_cwd, path) {
    if (path.endsWith('.xlsx')) return xlsxBlob;
    if (path.endsWith('.pptx')) return pptxBlobPromise;
    throw new Error(`Unexpected smoke path: ${path}`);
  },
};

function App() {
  const [path, setPath] = useState('wide.xlsx');
  const stableApi = useMemo(() => api, []);
  useEffect(() => {
    window.__setOfficeSmokePath = setPath;
  }, []);
  return (
    <div id="office-smoke-panel">
      <FilePreviewContent
        api={stableApi}
        cwd="C:/office-smoke"
        path={path}
        wrapPreview={false}
      />
    </div>
  );
}

const style = document.createElement('style');
style.textContent = `
  html, body, #root { height: 100%; margin: 0; }
  body { overflow: auto; background: #fff; }
  #office-smoke-panel {
    display: flex;
    flex-direction: column;
    width: 680px;
    height: 620px;
    border: 1px solid #999;
  }
`;
document.head.appendChild(style);
createRoot(document.getElementById('root')).render(<App />);

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
async function waitFor(predicate, timeout = 12_000) {
  const started = Date.now();
  while (Date.now() - started < timeout) {
    const value = predicate();
    if (value) return value;
    await delay(50);
  }
  throw new Error('Timed out waiting for Office preview smoke condition');
}

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function runSmoke() {
  const sheet = await waitFor(() => document.querySelector('.x-spreadsheet-sheet'));
  const panel = document.getElementById('office-smoke-panel');
  const initialSheetWidth = sheet.getBoundingClientRect().width;
  assert(Math.abs(initialSheetWidth - 680) < 4, `initial spreadsheet width ${initialSheetWidth}`);

  const horizontal = document.querySelector('.x-spreadsheet-scrollbar.horizontal');
  assert(horizontal, 'missing spreadsheet horizontal scrollbar');
  assert(getComputedStyle(horizontal).display !== 'none', 'spreadsheet horizontal scrollbar hidden');
  assert(horizontal.scrollWidth > horizontal.clientWidth, 'spreadsheet horizontal scrollbar has no draggable range');
  horizontal.scrollLeft = 260;
  horizontal.dispatchEvent(new Event('scroll', { bubbles: true }));
  await delay(100);
  assert(horizontal.scrollLeft > 0, 'spreadsheet horizontal thumb did not move');

  const spreadsheetShell = document.querySelector('.ace-office-preview-shell');
  assert(spreadsheetShell.querySelectorAll('.ace-office-preview-zoom-button').length === 2, 'missing spreadsheet zoom buttons');
  spreadsheetShell.dispatchEvent(new WheelEvent('wheel', {
    bubbles: true,
    cancelable: true,
    ctrlKey: true,
    deltaY: -120,
  }));
  await delay(150);
  const spreadsheetRoot = document.querySelector('.x-spreadsheet');
  assert(spreadsheetRoot, 'spreadsheet root disappeared after Ctrl-wheel');
  assert(Number(spreadsheetRoot.style.zoom) === 1.1, `spreadsheet Ctrl-wheel zoom ${spreadsheetRoot.style.zoom}`);
  spreadsheetShell.dispatchEvent(new WheelEvent('wheel', {
    bubbles: true,
    cancelable: true,
    ctrlKey: true,
    deltaY: 120,
  }));
  await delay(150);
  assert(Number(spreadsheetRoot.style.zoom) === 1, `spreadsheet Ctrl-wheel zoom reset ${spreadsheetRoot.style.zoom}`);

  panel.style.width = '1040px';
  window.dispatchEvent(new Event('resize'));
  await delay(350);
  const liveSheet = document.querySelector('.x-spreadsheet-sheet');
  const resizedSheetWidth = liveSheet.getBoundingClientRect().width;
  const spreadsheetHost = document.querySelector('.ace-side-spreadsheet-preview');
  assert(
    Math.abs(resizedSheetWidth - 1040) < 4,
    `resized spreadsheet width ${resizedSheetWidth}; panel ${panel.clientWidth}; host ${spreadsheetHost.clientWidth}`,
  );

  window.__setOfficeSmokePath('slides.pptx');
  const next = await waitFor(() => {
    const failure = document.querySelector('.ace-side-presentation-status .text-danger');
    if (failure) throw new Error(`PPTX render failed: ${failure.textContent}`);
    return document.querySelector('.ace-side-presentation-nav.is-next:not(:disabled)');
  });
  const previous = document.querySelector('.ace-side-presentation-nav.is-previous');
  assert(previous && previous.disabled, 'PPTX previous control should start disabled');
  assert(document.querySelectorAll('.ace-side-presentation-nav').length === 2, 'PPTX edge masks missing');
  next.click();
  await waitFor(() => !previous.disabled);

  const frame = document.querySelector('.ace-side-presentation-frame');
  const initialFrameWidth = frame.getBoundingClientRect().width;
  panel.style.width = '760px';
  await delay(350);
  const resizedFrameWidth = frame.getBoundingClientRect().width;
  assert(initialFrameWidth > resizedFrameWidth + 250, 'PPTX frame did not follow panel resize');
  assert(Math.abs(resizedFrameWidth - 760) < 4, `resized PPTX frame width ${resizedFrameWidth}`);

  return {
    spreadsheet: {
      initialSheetWidth,
      resizedSheetWidth,
      horizontalClientWidth: horizontal.clientWidth,
      horizontalScrollWidth: horizontal.scrollWidth,
      horizontalScrollLeft: horizontal.scrollLeft,
      zoom: Number(spreadsheetRoot.style.zoom),
    },
    presentation: {
      initialFrameWidth,
      resizedFrameWidth,
      previousEnabledAfterNext: !previous.disabled,
      edgeControls: document.querySelectorAll('.ace-side-presentation-nav').length,
    },
  };
}

runSmoke().then((result) => {
  const output = document.createElement('pre');
  output.id = 'office-smoke-result';
  output.dataset.status = 'pass';
  output.textContent = JSON.stringify(result);
  document.body.appendChild(output);
  void fetch('http://127.0.0.1:4318/result', {
    method: 'POST',
    mode: 'no-cors',
    headers: { 'Content-Type': 'text/plain' },
    body: JSON.stringify({ status: 'pass', result }),
  });
}).catch((error) => {
  const output = document.createElement('pre');
  output.id = 'office-smoke-result';
  output.dataset.status = 'fail';
  output.textContent = error?.stack || String(error);
  document.body.appendChild(output);
  void fetch('http://127.0.0.1:4318/result', {
    method: 'POST',
    mode: 'no-cors',
    headers: { 'Content-Type': 'text/plain' },
    body: JSON.stringify({
      status: 'fail',
      error: error?.stack || String(error),
      debug: {
        spreadsheetRoots: document.querySelectorAll('.x-spreadsheet').length,
        spreadsheetSheets: document.querySelectorAll('.x-spreadsheet-sheet').length,
        officeShells: document.querySelectorAll('.ace-office-preview-shell').length,
        presentationFrames: document.querySelectorAll('.ace-side-presentation-frame').length,
        setPathType: typeof window.__setOfficeSmokePath,
      },
    }),
  });
});
