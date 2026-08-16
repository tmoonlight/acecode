import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const source = (relativePath) => fs.readFileSync(path.resolve(here, relativePath), 'utf8');

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('main installs startup bootstrap and FCP reporting before React mount', () => {
  const main = source('../main.jsx');
  const reportIndex = main.indexOf("reportDesktopStartupMilestone('web_bootstrap')");
  const paintIndex = main.indexOf('installDesktopStartupPaintReporter()');
  const mountIndex = main.indexOf("createRoot(document.getElementById('root'))");
  assert.ok(reportIndex > 0 && reportIndex < mountIndex);
  assert.ok(paintIndex > reportIndex && paintIndex < mountIndex);
});

run('App subscribes to native progress and renders a Desktop-only logo status screen', () => {
  const app = source('../App.jsx');
  assert.match(app, /initialDesktopStartupProgress\(\)/);
  assert.match(app, /subscribeDesktopStartupProgress\(handleProgress\)/);
  assert.match(app, /desktopModeRef\.current === 'shell'/);
  assert.match(app, /data-desktop-startup-screen="true"/);
  assert.match(app, /<InteractiveHomeLogo[\s\S]*?enabled=\{false\}/);
  assert.match(app, /reportDesktopStartupMilestone\('daemon_connecting'\)/);
  assert.match(app, /reportDesktopStartupMilestone\('daemon_connected'\)/);
  assert.match(app, /reportDesktopStartupMilestone\('ui_ready'\)/);
});

run('startup status stays in the checking screen and never enters the main ChatView', () => {
  const app = source('../App.jsx');
  const chat = source('../components/ChatView.jsx');
  const styles = source('../styles/globals.css');
  assert.match(styles, /\.ace-desktop-startup-screen\s*\{/);
  assert.doesNotMatch(app, /desktopStartupStatus=\{desktopStartupStatus\}/);
  assert.doesNotMatch(chat, /desktopStartupStatus|data-desktop-startup-status/);
  assert.doesNotMatch(styles, /\.ace-home-startup-status|\.ace-desktop-startup-elapsed/);
  assert.doesNotMatch(app, /formatDesktopStartupElapsed|DESKTOP_STARTUP_TERMINAL_VISIBLE_MS/);
});

run('startup status uses a scoped Windows UI font and subpixel rasterization', () => {
  const styles = source('../styles/globals.css');
  const statusBlock = styles.match(/\.ace-desktop-startup-status\s*\{([\s\S]*?)\}/)?.[1] || '';
  assert.match(statusBlock, /font-family:\s*"Segoe UI Variable Text", "Microsoft YaHei UI"/);
  assert.match(statusBlock, /font-size:\s*14px;/);
  assert.match(statusBlock, /-webkit-font-smoothing:\s*subpixel-antialiased;/);
  assert.match(statusBlock, /text-rendering:\s*auto;/);
});

run('native splash composites grayscale text coverage and never formats visible time', () => {
  const splash = source('../../../src/desktop/splash_screen.cpp');
  assert.match(splash, /ANTIALIASED_QUALITY/);
  assert.match(splash, /composite_grayscale_text\(/);
  assert.match(splash, /premultiplied_bgra\(/);
  assert.doesNotMatch(splash, /CLEARTYPE_QUALITY|format_status_text|setprecision/);
});

run('native bridge injects snapshots and keeps pageReady as the visibility gate', () => {
  const nativeMain = source('../../../src/desktop/main.cpp');
  const progressHeader = source('../../../src/desktop/startup_progress.hpp');
  assert.match(nativeMain, /window\.__ACECODE_DESKTOP_STARTUP__/);
  assert.match(nativeMain, /aceDesktop_reportStartupMilestone/);
  assert.match(nativeMain, /kDesktopStartupProgressEvent/);
  assert.match(progressHeader, /acecode:desktop-startup-progress/);
  assert.match(
    nativeMain,
    /host\.bind\("aceDesktop_pageReady"[\s\S]*?mark_startup\("dom_ready", "frontend"\)[\s\S]*?close_splash_once\(\)/,
  );
});
