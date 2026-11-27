import assert from 'node:assert/strict';
import {
  annotationPageMissingError,
  registerAnnotationEditor,
  requestAnnotationEdit,
} from './annotationEdit.js';

const page = (pageId, url) => ({ pageId, url, title: pageId });
const record = { id: 'record-1', pageId: 'page-1', pageUrl: 'https://example.test/a', number: 1 };

{
  const calls = [];
  const cleanup = registerAnnotationEditor('page-1', async (value) => {
    calls.push(value);
    return { ok: true, background: true };
  });
  assert.deepEqual(await requestAnnotationEdit('page-1', record), {
    ok: true,
    background: true,
  });
  assert.deepEqual(calls, [record]);
  cleanup();
}

// The active pane owns the only mounted editor. An inactive tab has a native
// page id but no editor until the requested tab finishes mounting.
{
  const order = [];
  let cleanup = () => {};
  const result = await requestAnnotationEdit('page-2', { ...record, pageId: 'page-2' }, {
    url: 'https://example.test/b',
    pages: [page('page-2', 'https://example.test/b#section')],
    tabs: [{ type: 'browser', pageId: 'page-2' }],
    activate: async (pageId) => {
      order.push(`activate:${pageId}`);
      cleanup = registerAnnotationEditor(pageId, async (value) => {
        order.push(`edit:${value.pageId}`);
        return { ok: true };
      });
      return true;
    },
  });
  assert.deepEqual(result, { ok: true });
  assert.deepEqual(order, ['activate:page-2', 'edit:page-2']);
  cleanup();
}

// A URL match can recover the live native page when the record still carries a
// stale page id. The editor receives the record only after that page mounts.
{
  const activated = [];
  let cleanup = () => {};
  const result = await requestAnnotationEdit('retired-page-id', record, {
    url: 'https://example.test/recovered',
    pages: [page('page-live', 'https://example.test/recovered#details')],
    tabs: [{ type: 'browser', pageId: 'page-live' }],
    activate: (pageId) => {
      activated.push(pageId);
      cleanup = registerAnnotationEditor(pageId, async () => ({ ok: true, recovered: true }));
      return true;
    },
  });
  assert.equal(result.recovered, true);
  assert.deepEqual(activated, ['page-live']);
  cleanup();
}

{
  const missing = await requestAnnotationEdit('page-gone', {
    ...record,
    pageId: 'page-gone',
    pageUrl: 'https://example.test/gone',
  }, {
    pages: [page('page-live', 'https://example.test/live')],
    tabs: [{ type: 'browser', pageId: 'page-live' }],
    activate: () => {
      throw new Error('a missing page must not be activated');
    },
  });
  assert.equal(missing.ok, false);
  assert.equal(missing.reason, 'missing_page');
  assert.equal(missing.error, annotationPageMissingError());
}

{
  const result = await requestAnnotationEdit('page-3', { ...record, pageId: 'page-3' }, {
    pages: [page('page-3', 'https://example.test/c')],
    activate: () => false,
  });
  assert.equal(result.ok, false);
  assert.equal(result.reason, 'activation_failed');
  assert.notEqual(result.error, annotationPageMissingError());
}

console.log('[pass] annotation editing resolves and activates an existing unfocused tab');
