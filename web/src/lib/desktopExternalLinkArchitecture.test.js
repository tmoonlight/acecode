import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(srcRoot, '..', '..');

function source(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8');
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

run('App installs the Desktop-wide external link router with visible failure feedback', () => {
  const app = source('web/src/App.jsx');

  assert.match(app, /import \{ installDesktopExternalLinkRouter \} from '\.\/lib\/externalUrl\.js';/);
  assert.match(
    app,
    /useEffect\(\(\) => installDesktopExternalLinkRouter\(\{[\s\S]*?desktop\.externalBrowserOpenFailed[\s\S]*?\}\), \[t\]\);/,
  );
});

run('main WebView2 marks HTTP new windows handled before native launch', () => {
  const host = source('src/desktop/web_host.cpp');
  const start = host.indexOf('core->add_NewWindowRequested');
  const end = host.indexOf('&win_token', start);
  const handler = host.slice(start, end);

  assert.ok(start >= 0 && end > start, 'main WebView2 new-window handler must exist');
  assert.match(host, /#include "external_url\.hpp"/);
  assert.match(handler, /win_is_file_uri\(u\)[\s\S]*dispatch_file_uri\(u\)/);

  const validation = handler.indexOf('is_safe_external_url(external_url)');
  const handled = handler.indexOf('args->put_Handled(TRUE)', validation);
  const launch = handler.indexOf('open_external_url(external_url)', validation);
  const failureLog = handler.indexOf('failed to open new-window URL', launch);

  assert.ok(validation >= 0, 'new-window URL must pass the HTTP(S) validator');
  assert.ok(handled > validation, 'validated URL must be marked handled');
  assert.ok(launch > handled, 'handled must be set before the OS launcher runs');
  assert.ok(failureLog > launch, 'native launcher failures must be logged');
});

run('the Agent Browser keeps its separate new-window implementation', () => {
  const host = source('src/desktop/web_host.cpp');
  const agentHost = source('src/desktop/agent_browser_host.cpp');

  assert.match(host, /install_win_webview_navigation_handlers/);
  assert.match(agentHost, /page->webview->add_NewWindowRequested/);
  assert.doesNotMatch(agentHost, /open_external_url\(external_url\)/);
});
