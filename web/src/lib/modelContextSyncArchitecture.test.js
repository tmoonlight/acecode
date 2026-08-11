import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('saved model edits refresh the mounted active-session model state', () => {
  const app = source('App.jsx');
  const settings = source('components/SettingsPage.jsx');
  const modelSettings = source('components/model-settings/ModelSettingsSection.jsx');
  const chat = source('components/ChatView.jsx');

  assert.match(settings, /<ModelSettingsSection onModelProfileUpdated=\{onModelProfileUpdated\}/);
  assert.match(modelSettings, /const updated = await api\.updateModel\(originalName, payloads\[0\]\);/);
  assert.match(modelSettings, /announceMutation\(updated\);/);
  assert.match(modelSettings, /onModelProfileUpdated\?\.\(safe\);/);
  assert.match(app, /modelProfileRevision=\{modelProfileRevision\}/);
  assert.match(
    app,
    /onModelProfileUpdated=\{\(\) => setModelProfileRevision\(\(value\) => value \+ 1\)\}/,
  );
  assert.match(chat, /\[api, modelProfileRevision, ref\?\.context_window,/);
});
