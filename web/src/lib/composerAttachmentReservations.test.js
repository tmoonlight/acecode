import assert from 'node:assert/strict';
import {
  clearComposerAttachmentReservations,
  createComposerAttachmentReservations,
  releaseComposerAttachmentFile,
  reserveComposerAttachmentFiles,
} from './composerAttachmentReservations.js';
import { markFileSourcePath } from './composerFileTransfer.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function browserFile(name, lastModified = 10) {
  return { name, size: 4, type: 'text/plain', lastModified };
}

function localIds() {
  let next = 0;
  return () => `local-test-${++next}`;
}

run('attachment reservations deduplicate a batch and later additions', () => {
  const reservations = createComposerAttachmentReservations();
  const createLocalId = localIds();
  const first = markFileSourcePath(browserFile('notes.txt'), 'C:\\work\\notes.txt');
  const same = markFileSourcePath(browserFile('renamed.txt'), 'c:/WORK/NOTES.TXT');
  const other = browserFile('other.txt', 11);

  const accepted = reserveComposerAttachmentFiles(
    reservations,
    [first, same, other],
    createLocalId,
  );
  assert.equal(accepted.length, 2);
  assert.equal(accepted[0].file, first);
  assert.equal(accepted[1].file, other);
  assert.deepEqual(
    reserveComposerAttachmentFiles(reservations, [same, other], createLocalId),
    [],
  );
});

run('attachment reservation release allows the same file to be added again', () => {
  const reservations = createComposerAttachmentReservations();
  const createLocalId = localIds();
  const file = browserFile('retry.txt');
  const [reserved] = reserveComposerAttachmentFiles(reservations, [file], createLocalId);

  assert.equal(releaseComposerAttachmentFile(reservations, reserved.localId), true);
  assert.equal(releaseComposerAttachmentFile(reservations, reserved.localId), false);
  assert.equal(reserveComposerAttachmentFiles(reservations, [file], createLocalId).length, 1);
});

run('clearing attachment reservations releases every composer file', () => {
  const reservations = createComposerAttachmentReservations();
  const createLocalId = localIds();
  const files = [browserFile('a.txt'), browserFile('b.txt')];
  reserveComposerAttachmentFiles(reservations, files, createLocalId);

  clearComposerAttachmentReservations(reservations);

  assert.equal(reserveComposerAttachmentFiles(reservations, files, createLocalId).length, 2);
});
