import assert from 'node:assert/strict';
import fs from 'node:fs';

assert.ok(process.env.ACECODE_TEST_ANNOTATION_SESSION && fs.existsSync(process.env.ACECODE_TEST_ANNOTATION_SESSION),
  'Build the test-support native driver and set ACECODE_TEST_ANNOTATION_SESSION; integration cannot be skipped');
await import('./pageAnnotationStyles.browser.test.js');
