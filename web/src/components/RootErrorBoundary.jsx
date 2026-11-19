import { Component } from 'react';
import {
  planRootErrorRecovery,
  readRootErrorAutoReloadAt,
  writeRootErrorAutoReloadAt,
} from '../lib/rootErrorRecovery.js';

// 入口最外层的错误边界。没有它时,任何一处没被接住的渲染错误都会让 React 卸掉整棵
// 界面,窗口只剩底色(桌面版里连刷新的入口都没有)。
//
// 兜底界面刻意只用原生标签和内联样式:不依赖主题上下文、懒加载块或其它组件,
// 这样它自己不会再出错。判定逻辑见 lib/rootErrorRecovery.js。
//
// 唯一的运行时依赖是文案:构建时中文字面量会被换成翻译函数调用。翻译本身出错时
// 落回英文字面量(见 copy),兜底界面照样画得出来。

function copy(read, fallback) {
  try {
    const text = read();
    return typeof text === 'string' && text ? text : fallback;
  } catch {
    return fallback;
  }
}

function sessionStore() {
  try {
    return window.sessionStorage;
  } catch {
    return null;
  }
}

function reloadPage() {
  window.location.reload();
}

const containerStyle = {
  position: 'fixed',
  inset: 0,
  display: 'flex',
  flexDirection: 'column',
  alignItems: 'center',
  justifyContent: 'center',
  gap: 16,
  background: 'var(--ace-bg, #f5f5f2)',
  color: 'var(--ace-fg, #1a1a1a)',
  font: '14px/1.5 system-ui, sans-serif',
};

const buttonStyle = {
  padding: '6px 16px',
  border: '1px solid var(--ace-border, #e2e2de)',
  borderRadius: 6,
  background: 'var(--ace-surface, #ffffff)',
  color: 'inherit',
  font: 'inherit',
  cursor: 'pointer',
};

export class RootErrorBoundary extends Component {
  state = { failed: false, reloading: false };

  static getDerivedStateFromError(error) {
    const plan = planRootErrorRecovery({
      error,
      now: Date.now(),
      lastAutoReloadAt: readRootErrorAutoReloadAt(sessionStore()),
    });
    return { failed: true, reloading: plan === 'reload' };
  }

  componentDidCatch(error, info) {
    // 桌面壳把 console.error 写进 desktop 日志;带上组件栈,事后凭日志就能定位。
    console.error(
      '[root-error-boundary]',
      String(error?.stack || error),
      String(info?.componentStack || ''),
    );
    if (!this.state.reloading) return;
    if (writeRootErrorAutoReloadAt(sessionStore(), Date.now())) {
      reloadPage();
    } else {
      // 记不下「已经自动重载过」就不自动重载,否则可能无限刷新。
      this.setState({ reloading: false });
    }
  }

  render() {
    if (!this.state.failed) return this.props.children;
    if (this.state.reloading) return null;
    return (
      <div role="alert" style={containerStyle}>
        <div>{copy(() => '界面出错了', 'Something went wrong')}</div>
        <button type="button" style={buttonStyle} onClick={reloadPage} autoFocus>
          {copy(() => '重新加载', 'Reload')}
        </button>
      </div>
    );
  }
}
