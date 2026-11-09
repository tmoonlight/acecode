import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createPhoneSettingsStore } from './androidPhoneSettings.js';

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

// A resume refresh cannot race a write, even after its view unsubscribes.
{
  const calls = [];
  const write = deferred();
  const store = createPhoneSettingsStore(async (action, fields) => {
    calls.push([action, fields]);
    return action === 'pet' ? write.promise : { pet: 'rover', overlay: true };
  });
  let notices = 0;
  const unsubscribe = store.subscribe(() => { notices += 1; });
  const saving = store.setPet('rover');
  const refreshing = store.refresh();
  unsubscribe();
  const before = notices;
  await Promise.resolve();
  assert.deepEqual(calls, [['pet', { kind: 'rover' }]]);
  assert.equal(store.getSnapshot().pending, 2);
  write.resolve({ pet: 'rover', overlay: true });
  await Promise.all([saving, refreshing]);
  assert.equal(calls[1][0], 'status');
  assert.equal(store.getSnapshot().status.pet, 'rover');
  assert.equal(store.getSnapshot().pending, 0);
  assert.equal(notices, before);
  console.log('[pass] Android setting writes survive unmount and serialize resume refresh');
}

// Failed saves retain the selected pet; retry repeats the write, not a GET.
{
  const calls = [];
  let fail = true;
  let refreshFails = false;
  const store = createPhoneSettingsStore(async (action, fields) => {
    calls.push([action, fields]);
    if (action === 'status' && refreshFails) throw new Error('refresh failed');
    if (action === 'pet' && fail) throw new Error('bridge unavailable');
    return { pet: action === 'pet' ? fields.kind : 'off', overlay: true };
  });
  assert.equal(await store.setPet('whale'), false);
  await store.refresh();
  assert.equal(store.getSnapshot().draftPet, 'whale');
  assert.equal(store.getSnapshot().error, 'bridge unavailable');
  refreshFails = true;
  await store.refresh();
  assert.equal(store.getSnapshot().failed.action, 'pet');
  assert.equal(store.getSnapshot().error, 'bridge unavailable');
  fail = false;
  assert.equal(await store.retry(), true);
  assert.deepEqual(calls.at(-1), ['pet', { kind: 'whale' }]);
  assert.equal(store.getSnapshot().draftPet, null);
  assert.equal(store.getSnapshot().error, '');
  assert.equal(store.getSnapshot().status.pet, 'whale');
  console.log('[pass] Android failed selection persists and retry resends the failed mutation');
}

// Native returns pet=off while overlay permission is still missing.
{
  let allowed = false;
  const store = createPhoneSettingsStore(async () => ({ pet: allowed ? 'clippy' : 'off', overlay: allowed }));
  await store.setPet('clippy');
  assert.equal(store.getSnapshot().status.pet, 'off');
  assert.equal(store.getSnapshot().draftPet, 'clippy');
  await store.refresh();
  assert.equal(store.getSnapshot().draftPet, 'clippy');
  allowed = true;
  await store.refresh();
  assert.equal(store.getSnapshot().draftPet, null);
  assert.equal(store.getSnapshot().status.pet, 'clippy');
  console.log('[pass] Android pending permission keeps the desired pet until resume confirms authorization');
}

// Newer choices remain selected while an older queued operation completes.
{
  const old = deferred();
  const latest = deferred();
  const store = createPhoneSettingsStore((action, fields) => fields.kind === 'whale' ? old.promise : latest.promise);
  const first = store.setPet('whale');
  const second = store.setPet('rover');
  old.resolve({ pet: 'whale', overlay: true });
  await first;
  assert.equal(store.getSnapshot().draftPet, 'rover');
  latest.resolve({ pet: 'rover', overlay: true });
  await second;
  assert.equal(store.getSnapshot().draftPet, null);
  assert.equal(store.getSnapshot().status.pet, 'rover');
  console.log('[pass] Android queued edits are not overwritten by older replies');
}

{
  const store = createPhoneSettingsStore(async () => ({ accessibility: false, pet: 'off', overlay: false }));
  await store.accessibility();
  assert.equal(store.getSnapshot().status.accessibility, false);
  assert.equal(store.getSnapshot().pending, 0);
  console.log('[pass] Android control state reflects native authorization, not the requested toggle value');
}

{
  const source = readFileSync(new URL('../components/AndroidPhoneSettings.jsx', import.meta.url), 'utf8');
  assert.ok(source.includes('data-settings-surface="true"'));
  assert.ok(source.includes('role="radiogroup"'));
  assert.ok(source.includes('aria-checked={selected === item.id}'));
  for (const path of ['classic/clippy/preview.png', 'classic/rover/preview.png', 'whale/preview.png']) assert.ok(source.includes(path));
  assert.ok(source.includes('settings.retry()'));
  assert.ok(!source.includes('<select'));
  assert.ok(!source.includes('font-semibold'));
  console.log('[pass] Android settings reuse ACECode tool rows and original pet preview artwork');
}

{
  const native = (path) => readFileSync(new URL(`../../../android/app/src/main/${path}`, import.meta.url), 'utf8');
  for (const folder of ['values', 'values-night']) {
    const colors = native(`res/${folder}/colors.xml`);
    for (const token of ['ace_bg', 'ace_surface', 'ace_border', 'ace_fg', 'ace_fg_muted', 'ace_accent']) {
      assert.ok(colors.includes(`name="${token}"`));
    }
  }
  const theme = native('res/values/themes.xml');
  assert.ok(!/#[0-9a-f]{6}/i.test(theme));
  const approval = native('java/dev/acecode/android/PhoneApproval.kt');
  assert.ok(approval.includes('FLAG_NOT_FOCUSABLE'));
  assert.ok(approval.includes('FLAG_NOT_TOUCH_MODAL'));
  assert.ok(approval.includes('ace_card_max_width'));
  assert.ok(approval.includes('WindowInsets.Type.displayCutout()'));
  assert.ok(approval.includes('pending?.optString("requestId") == id'));
  const buttons = native('res/layout/phone_approval.xml');
  assert.equal((buttons.match(/android:layout_height="match_parent"/g) || []).length, 2);
  assert.ok(buttons.includes('Widget.ACECode.Button.Primary'));
  console.log('[pass] Android native surfaces use shared light/night resources and bounded non-focusing approval controls');
}
