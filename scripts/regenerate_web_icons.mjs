#!/usr/bin/env node

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { INTERFACE_ICONS, interfaceIconSvg } from '../web/src/lib/interfaceIcons.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const OUT_DIR = path.join(ROOT, 'web', 'public', 'vs-icons');
const check = process.argv.includes('--check');
const failures = [];

fs.mkdirSync(OUT_DIR, { recursive: true });
for (const name of Object.keys(INTERFACE_ICONS).sort()) {
  const file = path.join(OUT_DIR, `${name}.svg`);
  const content = interfaceIconSvg(name);
  if (check) {
    // Windows Git checkouts may use CRLF; compare the generated SVG content.
    if (!fs.existsSync(file) || fs.readFileSync(file, 'utf8').replace(/\r\n/g, '\n') !== content) failures.push(name);
  } else {
    fs.writeFileSync(file, content, 'utf8');
  }
}
for (const file of fs.readdirSync(OUT_DIR).filter((name) => name.endsWith('.svg'))) {
  if (!Object.hasOwn(INTERFACE_ICONS, path.basename(file, '.svg'))) failures.push(`${file} (unregistered)`);
}
if (failures.length) {
  console.error(`Interface icon assets differ from the canonical family: ${failures.join(', ')}`);
  process.exitCode = 1;
} else {
  console.log(`${check ? 'Verified' : 'Generated'} ${Object.keys(INTERFACE_ICONS).length} rounded currentColor interface icons.`);
}
