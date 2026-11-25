// 自动模式（试验）的界面核对:真实 daemon + 假模型服务 + 无头浏览器打开 web/dist。
// Run from web (after `pnpm build`):
//   ACE_DAEMON_BINARY=<acecode 可执行文件> node scripts/test-auto-review-mode.mjs
// ACE_PLAYWRIGHT_MODULE / ACE_CHROMIUM_EXECUTABLE select optional browser tools.
// ACE_AUTO_REVIEW_TEST_OUTPUT optionally saves screenshots.
// 核对的内容:
//   1. 输入框的权限模式显示「自动模式（试验）」;
//   2. 连续三次被拦后第四次弹确认,确认框里能看到裁决的理由;
//   3. 裁决进行中显示「正在进行安全检查」,结束后工具行正常;
//   4. 设置 > 工具 的卡片能选裁决模型,改回「跟随当前模型」后 config.json 不再有该段;
//   5. 全程没有页面脚本报错(组件漏 import 会在这里暴露)。
// 假模型服务与验收脚本共用: tests/scripts/auto_review_acceptance_test.py --serve-fake <端口>。
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const web = fileURLToPath(new URL('../', import.meta.url));
const repo = path.resolve(web, '..');
const binary = process.env.ACE_DAEMON_BINARY;
if (!binary) throw new Error('set ACE_DAEMON_BINARY to a built acecode executable');
const modulePath = process.env.ACE_PLAYWRIGHT_MODULE;
const { chromium } = await import(modulePath ? pathToFileURL(path.resolve(modulePath)).href : 'playwright');
const outDir = process.env.ACE_AUTO_REVIEW_TEST_OUTPUT || '';

const results = [];
const check = (name, ok, detail = '') => {
  results.push([name, !!ok]);
  console.log((ok ? '[pass] ' : '[FAIL] ') + name + (ok ? '' : ' :: ' + String(detail).slice(0, 500)));
};
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const freePort = () => new Promise((resolve) => {
  const server = net.createServer();
  server.listen(0, '127.0.0.1', () => { const { port } = server.address(); server.close(() => resolve(port)); });
});
async function http(method, url, body) {
  const response = await fetch(url, { method, headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined });
  const text = await response.text();
  try { return [response.status, JSON.parse(text)]; } catch { return [response.status, text]; }
}
const shot = async (target, name) => {
  if (!outDir) return;
  fs.mkdirSync(outDir, { recursive: true });
  await target.screenshot({ path: path.join(outDir, name) });
};

// 短的真实路径,避开目录联接与 MAX_PATH。
const tempBase = process.platform === 'win32' ? path.parse(path.resolve(binary)).root : fs.realpathSync(process.env.TMPDIR || '/tmp');
const root = fs.mkdtempSync(path.join(tempBase, 'ace-review-ui-'));
const pkg = path.join(root, 'package');
fs.mkdirSync(path.join(pkg, 'share/acecode'), { recursive: true });
const staged = path.join(pkg, path.basename(binary));
fs.copyFileSync(binary, staged);
if (process.platform !== 'win32') fs.chmodSync(staged, 0o755);
for (const name of ['seed', 'models_dev']) {
  fs.cpSync(path.join(repo, 'assets', name), path.join(pkg, 'share/acecode', name), { recursive: true });
}
const profile = path.join(root, 'profile');
const workspace = path.join(root, 'workspace');
fs.mkdirSync(path.join(profile, '.acecode'), { recursive: true });
fs.mkdirSync(path.join(profile, 'AppData'));
fs.mkdirSync(workspace);

const modelPort = await freePort();
const base = `http://127.0.0.1:${modelPort}`;
const saved = ['main', 'reviewer'].map((name) => ({ name, provider: 'openai', base_url: base + '/v1',
  api_key: 'fixture', model: name + '-model' }));
const configPath = path.join(profile, '.acecode', 'config.json');
// 沙盒关掉:需要确认的命令才会落进「要确认」那一档,交给裁决。
fs.writeFileSync(configPath, JSON.stringify({ saved_models: saved, default_model_name: 'main',
  sandbox: { enabled: false }, migrations: { disable_sandbox_once: true }, network: { proxy_mode: 'off' },
  auto_review: { model_name: 'reviewer' } }, null, 2));
const env = { ...process.env, HOME: profile, USERPROFILE: profile, APPDATA: path.join(profile, 'AppData'),
  LOCALAPPDATA: path.join(profile, 'AppData'), NO_PROXY: '127.0.0.1,localhost', HTTP_PROXY: '', HTTPS_PROXY: '', ALL_PROXY: '' };

const fake = spawn(process.platform === 'win32' ? 'python' : 'python3',
  ['-I', path.join(repo, 'tests/scripts/auto_review_acceptance_test.py'), '--serve-fake', String(modelPort)], { stdio: 'ignore' });
const port = await freePort();
const url = `http://127.0.0.1:${port}`;
const log = fs.openSync(path.join(root, 'daemon.log'), 'w');
const daemon = spawn(staged, ['daemon', '--foreground', `--port=${port}`, '--token=ui-check-fixture',
  `--cwd=${workspace}`, `--run-dir=${path.join(root, 'run')}`, `--static-dir=${path.join(web, 'dist')}`],
  { cwd: workspace, env, stdio: ['ignore', log, log] });

const pageErrors = [];
const failedResponses = [];
let browser;
try {
  for (let i = 0; ; i += 1) {
    try { if ((await http('GET', url + '/api/health'))[0] === 200) break; } catch { /* 还没起来 */ }
    if (i > 120 || daemon.exitCode !== null) throw new Error('daemon did not start');
    await sleep(300);
  }
  const [, created] = await http('POST', url + '/api/sessions', { permission_mode: 'auto-review', model: 'main' });
  const sid = created.session_id;
  const idle = async () => {
    for (let i = 0; i < 200; i += 1) {
      const [, snapshot] = await http('GET', `${url}/api/sessions/${sid}/messages`);
      if (snapshot && !snapshot.busy) return snapshot;
      await sleep(300);
    }
    return null;
  };
  const send = async (text) => {
    const [status, body] = await http('POST', `${url}/api/sessions/${sid}/messages`, { text });
    if (status >= 300) throw new Error(`send failed ${status} ${JSON.stringify(body)}`);
    await sleep(400);
  };
  // 空会话还没有落盘记录,页面打不开它;先让第一条(会被拦下的)消息跑完。
  await send('RUN: git push --force origin main');
  await idle();

  browser = await chromium.launch({ headless: true, executablePath: process.env.ACE_CHROMIUM_EXECUTABLE || undefined });
  const page = await browser.newPage({ viewport: { width: 1360, height: 900 }, locale: 'zh-CN' });
  page.setDefaultTimeout(30000);
  page.on('pageerror', (error) => pageErrors.push(error.message));
  // 接口按设计返回的 404 不算脚本错误;地址单独记下来,缺脚本块才算失败。
  page.on('response', (response) => {
    if (response.status() >= 400) failedResponses.push(`${response.status()} ${response.url()}`);
  });
  page.on('console', (message) => {
    if (message.type() === 'error' && !message.text().startsWith('Failed to load resource')) pageErrors.push('console: ' + message.text());
  });
  await page.goto(`${url}/?open=${sid}&workspace=${created.workspace_hash}`, { waitUntil: 'domcontentloaded' });
  await page.locator('[data-slate-editor="true"]').first().waitFor();
  await sleep(1500);

  check('composer shows the mode name', await page.getByText('自动模式（试验）').count() > 0);

  for (const branch of ['dev', 'release']) {
    await send(`RUN: git push --force origin ${branch}`);
    await idle();
  }
  check('three blocked calls never opened a permission card',
    await page.locator('[data-permission-card="pending"]').count() === 0);
  await send('RUN: git push --force origin hotfix');
  const note = page.locator('[data-permission-review-note="true"]');
  await note.waitFor({ timeout: 40000 }).catch(() => {});
  const noteText = await note.count() ? await note.first().innerText() : '';
  check('fourth blocked call asks the user and shows the reviewer reason',
    noteText.includes('自动检查拦下了这个操作') && noteText.includes('B2: force-push rewrites published history'), noteText);
  await shot(page, 'permission_card.png');
  await page.getByRole('button', { name: '拒绝', exact: true }).first().click();
  await idle();
  check('permission card closes after the answer', await note.count() === 0);

  await send(`RUN: python -c "print('SLOW'+'REVIEW-done')" # SLOWREVIEW`);
  const reviewing = page.getByText('正在进行安全检查');
  const seen = await reviewing.first().waitFor({ timeout: 8000 }).then(() => true, () => false);
  check('progress shows the safety check while the reviewer is thinking', seen);
  if (seen) await shot(page, 'reviewing.png');
  const after = await idle();
  const lastTool = (after?.messages || []).filter((message) => message.role === 'tool').pop() || {};
  check('the reviewed command ran after the check', String(lastTool.content || '').includes('SLOWREVIEW-done'), lastTool.content);
  await sleep(800);
  check('the safety-check text is gone once the tool has run', await reviewing.count() === 0);

  await page.evaluate(() => window.dispatchEvent(new CustomEvent('acecode:open-setting', { detail: { section: 'tools' } })));
  const card = page.locator('[data-auto-review-settings="true"]');
  await card.waitFor();
  await card.scrollIntoViewIfNeeded();
  const select = page.locator('#auto-review-model');
  await page.waitForFunction(() => !document.querySelector('#auto-review-model')?.disabled);
  const options = await select.locator('option').allInnerTexts();
  check('settings card: label and options', (await card.innerText()).includes('自动模式（试验）')
    && options[0] === '跟随当前模型' && options.some((text) => text.includes('reviewer')), options);
  check('settings card: shows the configured reviewer', await select.inputValue() === 'reviewer', await select.inputValue());
  await shot(page, 'settings_page.png');
  await select.selectOption('');
  await sleep(1200);
  let [, settings] = await http('GET', url + '/api/config/auto-review');
  check('choosing the first option clears the setting', settings.model_name === '' && settings.configured === false, settings);
  check('config.json no longer has the auto_review section',
    !('auto_review' in JSON.parse(fs.readFileSync(configPath, 'utf8'))));
  await select.selectOption('reviewer');
  await sleep(1200);
  [, settings] = await http('GET', url + '/api/config/auto-review');
  check('choosing a saved model stores it', settings.model_name === 'reviewer'
    && JSON.parse(fs.readFileSync(configPath, 'utf8')).auto_review?.model_name === 'reviewer', settings);
  check('no error is shown in the card', await card.locator('[role="alert"]').count() === 0);

  check('no page errors', pageErrors.length === 0, pageErrors.join(' | '));
  check('no script or asset failed to load', failedResponses.every((line) => line.includes('/api/')), failedResponses.join(' | '));
} catch (error) {
  check('script completed', false, error.stack || error);
} finally {
  if (browser) await browser.close().catch(() => {});
  daemon.kill();
  fake.kill();
  fs.closeSync(log);
}
const failed = results.filter(([, ok]) => !ok).length;
console.log(`\n${results.length - failed}/${results.length} checks passed`);
if (failed) console.log(`daemon log: ${path.join(root, 'daemon.log')}`);
else fs.rmSync(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 });
process.exit(failed ? 1 : 0);
