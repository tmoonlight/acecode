import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dialog = fs.readFileSync(
  path.resolve(here, '../components/model-settings/ModelProfileDialog.jsx'),
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

run('高级设置展开后自动滚到弹窗正文的可见位置', () => {
  assert.match(dialog, /const advancedSectionRef = useRef\(null\);/);
  assert.match(
    dialog,
    /const toggleAdvancedSettings = \(\) => \{[\s\S]*?if \(advancedOpen\)[\s\S]*?setAdvancedOpen\(false\);[\s\S]*?setAdvancedOpen\(true\);[\s\S]*?window\.requestAnimationFrame\([\s\S]*?advancedSectionRef\.current[\s\S]*?scrollIntoView\(\{[\s\S]*?behavior: reduceMotion \? 'auto' : 'smooth',[\s\S]*?block: 'start',[\s\S]*?inline: 'nearest',/,
  );
  assert.match(dialog, /ref=\{advancedSectionRef\}[\s\S]*?className="scroll-mt-4/);
  assert.match(dialog, /onClick=\{toggleAdvancedSettings\}/);
  assert.match(dialog, /aria-controls="model-profile-advanced-settings"/);
  assert.match(dialog, /id="model-profile-advanced-settings"/);
});
