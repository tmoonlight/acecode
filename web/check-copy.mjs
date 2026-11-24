import fs from 'node:fs';
import path from 'node:path';
import { collectStaticCopy } from './scripts/i18n-audit.mjs';
import { sourceCatalogs } from './src/i18n/sourceCatalog.generated.js';
import { OPAQUE_STATIC_COPY } from './src/i18n/sourceAllowlist.js';
const cat = new Set(Object.values(sourceCatalogs['zh-CN']));
const opaque = new Set(OPAQUE_STATIC_COPY);
const files = [];
(function walk(d) {
  for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const t = path.join(d, e.name);
    if (e.isDirectory()) walk(t);
    else if (/\.(js|jsx)$/.test(e.name)) files.push(t);
  }
})('src');
for (const f of files) {
  for (const r of collectStaticCopy(fs.readFileSync(f, 'utf8'), f)) {
    if (!cat.has(r.text) && !opaque.has(r.text)) console.log(`${f}:${r.line} ${JSON.stringify(r.text)}`);
  }
}
