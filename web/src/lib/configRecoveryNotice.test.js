import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  normalizeConfigRecoveryNotice,
  recoveryNoticeBlocksStartup,
} from './configRecoveryNotice.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function run(name, fn) {
  try {
    const result = fn();
    if (result && typeof result.then === 'function') {
      return result.then(
        () => console.log(`[pass] ${name}`),
        (error) => { console.error(`[fail] ${name}`); throw error; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('recovery notice normalization accepts only a pending sanitized payload', () => {
  assert.equal(normalizeConfigRecoveryNotice(null), null);
  assert.equal(normalizeConfigRecoveryNotice({ pending: false }), null);
  assert.deepEqual(normalizeConfigRecoveryNotice({
    pending: true,
    recovered_at_ms: 123,
    config_path: 'C:/Users/test/.acecode/config.json',
    invalid_backup_path: 'C:/Users/test/.acecode/config-backups/invalid/bad.json',
    invalid_backup_dir: 'C:/Users/test/.acecode/config-backups/invalid',
    ignored_secret: 'never-render-this',
  }), {
    pending: true,
    recoveredAtMs: 123,
    configPath: 'C:/Users/test/.acecode/config.json',
    invalidBackupPath: 'C:/Users/test/.acecode/config-backups/invalid/bad.json',
    invalidBackupDir: 'C:/Users/test/.acecode/config-backups/invalid',
  });
  assert.equal(recoveryNoticeBlocksStartup({ pending: true }, true), true);
  assert.equal(recoveryNoticeBlocksStartup({ pending: true }, false), false);
});

run('recovery notice API reads without consuming and explicitly acknowledges', () => {
  const source = fs.readFileSync(path.join(srcRoot, 'lib', 'api.js'), 'utf8');
  assert.match(source,
    /getConfigRecoveryNotice:\s*\(\)\s*=>\s*request\('GET',\s*'\/api\/config\/recovery-notice'/);
  assert.match(source,
    /acknowledgeConfigRecoveryNotice:\s*\(\)\s*=>\s*request\(\s*'POST',\s*'\/api\/config\/recovery-notice\/acknowledge'/);
});

run('App shows only the blocking recovery modal and keeps notice failures silent', () => {
  const app = fs.readFileSync(path.join(srcRoot, 'App.jsx'), 'utf8');
  const dialog = fs.readFileSync(
    path.join(srcRoot, 'components', 'ConfigRecoveryDialog.jsx'), 'utf8');
  const recoveryFlow = app.slice(
    app.indexOf('api.getConfigRecoveryNotice()'),
    app.indexOf('const pollUpdateJob'),
  );

  assert.match(app, /normalizeConfigRecoveryNotice/);
  assert.match(app, /configRecoveryNoticeChecked/);
  assert.match(app, /configRecoveryBlocking/);
  assert.match(app, /open=\{updateDialogOpen && !configRecoveryBlocking\}/);
  assert.match(app, /<ConfigRecoveryDialog/);
  assert.match(recoveryFlow, /api\.acknowledgeConfigRecoveryNotice\(\)/);
  assert.match(recoveryFlow, /setConfigRecoveryNoticeChecked\(true\)/);
  assert.doesNotMatch(recoveryFlow, /toast\s*\(/);
  assert.match(dialog, /<Modal[\s\S]*dismissOnBackdrop=\{false\}/);
  assert.match(dialog, /dismissOnEscape=\{false\}/);
  assert.match(dialog, /layerClassName="z-\[420\]"/);
  assert.match(dialog, /configRecovery\.title/);
  assert.match(dialog, /configRecovery\.message/);
  assert.match(dialog, /notice\.invalidBackupDir/);
  assert.doesNotMatch(dialog, /window\.(alert|confirm)/);
});
