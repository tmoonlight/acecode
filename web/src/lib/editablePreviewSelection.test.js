import assert from 'node:assert/strict';
import {
  slateDocumentText,
  slateSelectionDecorationModel,
} from './editablePreviewSelection.js';
import { createSelectionContext } from './selectionChatContext.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const document = [
  { type: 'heading', level: 1, children: [{ text: '标题' }] },
  { type: 'paragraph', children: [{ text: 'alpha ' }, { text: 'beta', bold: true }] },
];

run('Slate decoration text follows the same visible text-node order', () => {
  assert.equal(slateDocumentText(document), '标题alpha beta');
});

run('persisted rendered selections become Slate leaf ranges with annotation metadata', () => {
  const context = createSelectionContext({
    id: 'selection-1',
    text: 'beta',
    path: 'C:/repo/README.md',
    kind: 'markdown',
    view: 'rendered',
    startOffset: 8,
    endOffset: 12,
    annotations: [{ id: 'annotation-1', text: '保留这个批注' }],
  });
  const model = slateSelectionDecorationModel(document, {
    contexts: [context],
    sourcePath: 'C:/repo/README.md',
  });

  assert.equal(model.groups.length, 1);
  assert.equal(model.groups[0].anchor.status, 'resolved');
  assert.deepEqual(model.rangesByPath.get('1.1'), [{
    anchor: { path: [1, 1], offset: 0 },
    focus: { path: [1, 1], offset: 4 },
    selectionDecorationId: 'selection-1',
    selectionAnnotated: true,
  }]);
});

run('inactive Slate selections are split across leaf paths', () => {
  const model = slateSelectionDecorationModel(document, {
    inactiveSelection: {
      anchor: { path: [1, 0], offset: 3 },
      focus: { path: [1, 1], offset: 2 },
    },
  });
  assert.equal(model.rangesByPath.get('1.0')[0].inactiveSelection, true);
  assert.deepEqual(model.rangesByPath.get('1.0')[0].focus, { path: [1, 0], offset: 6 });
  assert.equal(model.rangesByPath.get('1.1')[0].inactiveSelection, true);
  assert.deepEqual(model.rangesByPath.get('1.1')[0].focus, { path: [1, 1], offset: 2 });
});
