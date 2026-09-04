import assert from 'node:assert/strict';
import {
  OFFICE_PREVIEW_ZOOM_DEFAULT,
  OFFICE_PREVIEW_ZOOM_MAX,
  OFFICE_PREVIEW_ZOOM_MIN,
  clampOfficePreviewZoom,
  officePreviewFitZoom,
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

run('Office fit zoom uses the limiting axis and stays within supported bounds', () => {
  assert.equal(officePreviewFitZoom(900, 700, 600, 1000), 0.7);
  assert.equal(officePreviewFitZoom(600, 900, 1000, 500), 0.6);
  assert.equal(officePreviewFitZoom(1000, 1000, 100, 100), OFFICE_PREVIEW_ZOOM_MAX);
  assert.equal(officePreviewFitZoom(100, 100, 1000, 1000), OFFICE_PREVIEW_ZOOM_MIN);
  assert.equal(officePreviewFitZoom(0, 700, 600, 1000), OFFICE_PREVIEW_ZOOM_DEFAULT);
  assert.equal(officePreviewFitZoom(868, 968, 794, 1185), 0.81);
});

run('Office renderer logical dimensions compensate for browser zoom', () => {
  assert.equal(officePreviewLogicalSize(900, 1.5), 600);
  assert.equal(officePreviewLogicalSize(900, 0.5), 1800);
  assert.equal(officePreviewLogicalSize(0, 1, 120), 120);
});
