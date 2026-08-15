import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
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

run('模型单选使用 radio，多选保留 checkbox', () => {
  assert.match(picker, /role=\{allowMultiple \? 'group' : 'radiogroup'\}/);
  assert.match(picker, /type=\{allowMultiple \? 'checkbox' : 'radio'\}/);
  assert.match(
    picker,
    /name=\{allowMultiple \? undefined : `model-catalog-selection-\$\{provider\.id\}`\}/,
  );
  assert.match(picker, /checked=\{selected\}/);
  assert.match(picker, /onChange=\{\(\) => updateSelectedModels\(model\.id, model\)\}/);
  assert.match(picker, /<label[\s\S]*?cursor-pointer[\s\S]*?<input/);
  assert.match(picker, /className="h-\[17px\] w-\[17px\] shrink-0 accent-accent"/);
  assert.doesNotMatch(picker, /aria-multiselectable=\{allowMultiple \|\| undefined\}/);
});
