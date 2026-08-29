import assert from 'node:assert/strict';
import {
  OFFICE_PREVIEW_ZOOM_DEFAULT,
  OFFICE_PREVIEW_ZOOM_MAX,
  OFFICE_PREVIEW_ZOOM_MIN,
  clampOfficePreviewZoom,
  officePreviewLogicalSize,
  officePreviewZoomForWheel,
  stepOfficePreviewZoom,
} from './officePreviewZoom.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Office preview zoom steps in tenths and stays within its bounds', () => {
  assert.equal(stepOfficePreviewZoom(1, 1), 1.1);
  assert.equal(stepOfficePreviewZoom(1, -1), 0.9);
  assert.equal(stepOfficePreviewZoom(OFFICE_PREVIEW_ZOOM_MAX, 1), OFFICE_PREVIEW_ZOOM_MAX);
  assert.equal(stepOfficePreviewZoom(OFFICE_PREVIEW_ZOOM_MIN, -1), OFFICE_PREVIEW_ZOOM_MIN);
  assert.equal(clampOfficePreviewZoom(Number.NaN), OFFICE_PREVIEW_ZOOM_DEFAULT);
});

run('Ctrl-wheel zooms while an ordinary wheel leaves zoom unchanged', () => {
  assert.equal(officePreviewZoomForWheel(1, { ctrlKey: true, deltaY: -120 }), 1.1);
  assert.equal(officePreviewZoomForWheel(1, { ctrlKey: true, deltaY: 120 }), 0.9);
  assert.equal(officePreviewZoomForWheel(1, { ctrlKey: false, deltaY: -120 }), 1);
  assert.equal(officePreviewZoomForWheel(1, { ctrlKey: true, deltaY: 0 }), 1);
});

run('Office renderer logical dimensions compensate for browser zoom', () => {
  assert.equal(officePreviewLogicalSize(900, 1.5), 600);
  assert.equal(officePreviewLogicalSize(900, 0.5), 1800);
  assert.equal(officePreviewLogicalSize(0, 1, 120), 120);
});
