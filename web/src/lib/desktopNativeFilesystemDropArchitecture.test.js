import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

function source(relativeUrl) {
  return readFileSync(fileURLToPath(new URL(relativeUrl, import.meta.url)), 'utf8');
}

const inputBar = source('../components/InputBar.jsx');
const consoleDock = source('../components/ConsoleDock.jsx');
const webHost = source('../../../src/desktop/web_host.cpp');
const desktopCmake = source('../../../cmake/acecode_desktop.cmake');

assert.match(inputBar, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)/);
assert.match(inputBar, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)[\s\S]{0,180}event\.preventDefault\(\)/);
assert.match(inputBar, /!NATIVE_FILE_DROP \|\| HOST_OS === 'windows'/);
assert.match(consoleDock, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)/);
assert.match(consoleDock, /postWindowsNativeFilesystemDrop\(event\.dataTransfer\)[\s\S]{0,180}event\.preventDefault\(\)/);
assert.match(consoleDock, /!NATIVE_DROP \|\| HOST_OS === 'windows'/);

assert.match(webHost, /acecode:native-filesystem-drop:v1/);
assert.match(webHost, /ICoreWebView2WebMessageReceivedEventArgs2/);
assert.match(webHost, /get_AdditionalObjects/);
assert.match(webHost, /ICoreWebView2File/);
assert.match(webHost, /g_file_drop_handler\(std::move\(paths\)\)/);
assert.match(desktopCmake, /set\(_acecode_webview2_sdk_version "1\.0\.4078\.44"\)/);
assert.match(
  desktopCmake,
  /set\(WEBVIEW_MSWEBVIEW2_VERSION "\$\{_acecode_webview2_sdk_version\}"/,
);

console.log('ok - Windows composer and terminal share one native filesystem drop batch bridge');
