import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { applyJbModeTransition, initialJbMode, JB_MODE_DEFAULT } from './jbMode.js';

const here = path.dirname(fileURLToPath(import.meta.url));

assert.equal(JB_MODE_DEFAULT, false);
assert.equal(initialJbMode(undefined), false);
assert.equal(initialJbMode(null), false);
assert.equal(initialJbMode(false), false);
assert.equal(initialJbMode(true), true);

const stored = { enabled: undefined };
const themes = [];
let waves = 0;
const effects = {
  persist: (enabled) => {
    stored.enabled = enabled;
    return stored.enabled;
  },
  setTheme: (theme) => { themes.push(theme); },
  startHeatWave: () => { waves += 1; },
};

assert.equal(initialJbMode(stored.enabled), false);
const turnedOn = await applyJbModeTransition({
  previous: initialJbMode(stored.enabled),
  next: true,
  ...effects,
});
assert.equal(turnedOn, true);
assert.equal(stored.enabled, true);
assert.deepEqual(themes, ['dark']);
assert.equal(waves, 1);
assert.equal(initialJbMode(stored.enabled), true);

const stillOn = await applyJbModeTransition({
  previous: true,
  next: true,
  ...effects,
});
assert.equal(stillOn, true);
assert.equal(waves, 1);
assert.deepEqual(themes, ['dark']);

const turnedOff = await applyJbModeTransition({
  previous: true,
  next: false,
  ...effects,
});
assert.equal(turnedOff, false);
assert.equal(stored.enabled, false);
assert.equal(initialJbMode(stored.enabled), false);
assert.equal(waves, 1);
assert.deepEqual(themes, ['dark']);

const settings = fs.readFileSync(path.join(here, '../components/DeveloperSettings.jsx'), 'utf8');
assert.match(settings, /applyJbModeTransition\(/);
assert.match(settings, /startFullscreenHeatWave/);
assert.match(settings, /initialJbMode\(state\.enabled\)/);
const heat = fs.readFileSync(path.join(here, 'fullscreenHeatWave.js'), 'utf8');
assert.match(heat, /export function startFullscreenHeatWave/);
assert.match(heat, /if \(!event\.repeat\) startFullscreenHeatWave\(\)/);

console.log('[pass] jb mode: default off, off→on persists dark theme and one heat wave, on→off persists off with no theme write or heat wave');
