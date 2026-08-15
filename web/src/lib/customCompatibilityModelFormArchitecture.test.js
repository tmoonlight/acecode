import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dialog = fs.readFileSync(
  path.resolve(here, '../components/model-settings/ModelProfileDialog.jsx'),
  'utf8',
);
const picker = fs.readFileSync(
  path.resolve(here, '../components/model-settings/ProviderCatalogPicker.jsx'),
  'utf8',
);

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('自定义兼容 API 的基础字段集中到 Provider 右侧详情区', () => {
  assert.match(dialog, /const customCompatibilityApi = isCustomOpenAiCompatibilityProvider\(provider\);/);
  assert.match(
    dialog,
    /directModelDetails=\{customCompatibilityApi \? \([\s\S]*?<CustomCompatibilityApiFields/,
  );
  assert.match(
    picker,
    /\{directModelIdInput \? \([\s\S]*?id="custom-openai-model-id"[\s\S]*?\{directModelDetails\}[\s\S]*?\) : \(/,
  );
  assert.match(dialog, /function CustomCompatibilityApiFields\([\s\S]*?Base URL[\s\S]*?API Key[\s\S]*?预设名称/);
  assert.match(dialog, /fieldLabel\('model-profile-name', '预设名称', true\)/);
  assert.match(dialog, /placeholder="为空时使用 Model ID"/);
});

run('自定义兼容 API 不在 Provider 选择器下方重复基础字段', () => {
  assert.match(dialog, /!policy\?\.managed && !customCompatibilityApi && \(/);
  assert.match(dialog, /editing && !customCompatibilityApi && \(/);
  assert.match(dialog, /!editing && !customCompatibilityApi && \(/);
});
