import assert from 'node:assert/strict';
import {
  BACK_DISMISSIBLE_SELECTOR,
  NARROW_LAYOUT_QUERY,
  handleBack,
  hasBackDismissibleLayer,
  installBackBridge,
  isAndroidApp,
  shouldUseNativeContextMenu,
} from './mobileShell.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

class FakeKeyboardEvent {
  constructor(type, init) { this.type = type; Object.assign(this, init); }
}

function fakeDocument(layers) {
  const dispatched = [];
  return {
    dispatched,
    defaultView: { KeyboardEvent: FakeKeyboardEvent },
    body: { dispatchEvent: (event) => { dispatched.push(event); return true; } },
    querySelectorAll: (selector) => {
      assert.equal(selector, BACK_DISMISSIBLE_SELECTOR);
      return layers.map((visible) => ({ getClientRects: () => (visible ? [{}] : []) }));
    },
  };
}

// 场景: 安卓 App 的 WebView 在 UA 末尾追加 ACECodeAndroid/<版本>;普通手机浏览器没有。
// 期望: 只认这个标记,不按「是安卓」或「是 WebView」泛化(手机浏览器远程访问不能被当成 App)。
test('android app is recognised only by the app UA mark', () => {
  assert.equal(isAndroidApp('Mozilla/5.0 (Linux; Android 15; wv) Chrome/131 Mobile ACECodeAndroid/1.0.22'), true);
  assert.equal(isAndroidApp('Mozilla/5.0 (Linux; Android 15) Chrome/131 Mobile Safari/537.36'), false);
  assert.equal(isAndroidApp(''), false);
  assert.equal(NARROW_LAYOUT_QUERY, '(max-width: 599px)');
});

// 场景: 安卓上长按 API key 输入框想粘贴。
// 期望: 手指长按落在输入框或已选中文字上时让系统工具条出来;鼠标右键仍走 ACECode 菜单;
//       手指长按空白处 / 会话行(没有输入框也没选中文字)也还走 ACECode 菜单。
// 回归: 2026-11-07 安卓 App 里所有输入框都粘贴不了 —— 全局右键拦截吞掉了长按,
//       自定义菜单的「粘贴」读剪贴板在 WebView 里又没有权限。
test('touch long-press on editable or selected text keeps the native toolbar', () => {
  assert.equal(shouldUseNativeContextMenu({ pointerType: 'touch', editable: true }), true);
  assert.equal(shouldUseNativeContextMenu({ pointerType: 'touch', hasSelection: true }), true);
  assert.equal(shouldUseNativeContextMenu({ firesTouchEvents: true, editable: true }), true);
  assert.equal(shouldUseNativeContextMenu({ pointerType: 'pen', editable: true }), true);
  assert.equal(shouldUseNativeContextMenu({ pointerType: 'mouse', editable: true }), false);
  assert.equal(shouldUseNativeContextMenu({ pointerType: 'touch' }), false);
  assert.equal(shouldUseNativeContextMenu(), false);
});

// 场景: 安卓返回键 / 返回手势。
// 期望: 还有可见的浮层(设置、对话框、侧栏浮层、菜单)时补发一次 Esc 并报告「已处理」,
//       App 就不退后台;没有浮层或浮层都不可见时报告「未处理」,由 App 退到后台。
// 回归: 2026-11-07 设置开着按返回,App 直接退到后台。
test('back closes the top layer only when one is visible', () => {
  assert.ok(BACK_DISMISSIBLE_SELECTOR.includes('.ace-composer-popup'));
  assert.ok(BACK_DISMISSIBLE_SELECTOR.includes('[data-composer-capability-menu="true"]'));
  const open = fakeDocument([false, true]);
  assert.equal(hasBackDismissibleLayer(open), true);
  assert.equal(handleBack(open), true);
  assert.equal(open.dispatched.length, 1);
  assert.equal(open.dispatched[0].type, 'keydown');
  assert.equal(open.dispatched[0].key, 'Escape');
  assert.equal(open.dispatched[0].bubbles, true);

  const closed = fakeDocument([false]);
  assert.equal(handleBack(closed), false);
  assert.equal(closed.dispatched.length, 0);
  assert.equal(handleBack(null), false);
});

// 场景: App 外壳按返回时执行 window.__aceHandleBack()。
// 期望: 桥函数只是 handleBack 的薄包装,结果原样返回给外壳。
test('back bridge exposes handleBack on the window', () => {
  const doc = fakeDocument([true]);
  const win = { document: doc };
  installBackBridge(win);
  assert.equal(win.__aceHandleBack(), true);
  assert.equal(doc.dispatched.length, 1);
});
