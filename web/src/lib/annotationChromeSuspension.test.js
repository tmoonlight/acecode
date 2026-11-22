import assert from 'node:assert/strict';
import {
  annotationChromeSuspended,
  setAnnotationChromeSuspended,
} from './annotationChromeSuspension.js';

const emitted = [];
const emit = value => emitted.push(value);

assert.equal(annotationChromeSuspended(), false, 'no blocker starts out released');

setAnnotationChromeSuspended('menu', true, emit);
assert.equal(annotationChromeSuspended(), true);
assert.deepEqual(emitted, [true], 'the first blocker suspends the page chrome once');

setAnnotationChromeSuspended('menu', true, emit);
assert.deepEqual(emitted, [true], 're-asserting the same blocker does not resend the command');

// A dialog opening on top of an open menu must not release the chrome when the
// menu closes, and the dialog closing afterwards must release it exactly once.
setAnnotationChromeSuspended('dialog', true, emit);
assert.deepEqual(emitted, [true], 'nested blockers do not resend a redundant suspend');
setAnnotationChromeSuspended('menu', false, emit);
assert.deepEqual(emitted, [true], 'closing one of two blockers keeps the chrome suspended');
assert.equal(annotationChromeSuspended(), true);
setAnnotationChromeSuspended('dialog', false, emit);
assert.deepEqual(emitted, [true, false], 'the last blocker releasing resumes the chrome');
assert.equal(annotationChromeSuspended(), false);

setAnnotationChromeSuspended('menu', false, emit);
assert.deepEqual(emitted, [true, false], 'releasing an unknown blocker stays silent');

setAnnotationChromeSuspended('menu', true);
assert.equal(annotationChromeSuspended(), true, 'emit is optional');
setAnnotationChromeSuspended('menu', false);

console.log('[pass] annotation chrome suspension composes nested menu and dialog blockers without redundant commands');
