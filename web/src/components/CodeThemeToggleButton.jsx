import { VsIcon } from './Icon.jsx';

// 与 lib/markdown.js 的 codeThemeToggleHtml() 同一个按钮:两个图标都画出来,显示哪个由
// globals.css 按 <html data-code-theme> 决定;点击由 lib/useCodeThemeToggle.js 统一处理。
export function CodeThemeToggleButton({ className = 'ace-code-action-btn', size = 14 }) {
  return (
    <button
      type="button"
      className={`${className} ace-code-theme-btn`}
      data-code-theme-toggle="true"
      aria-label="切换代码深浅色"
    >
      <VsIcon name="brightness" size={size} className="ace-code-theme-icon-light" />
      <VsIcon name="darkTheme" size={size} className="ace-code-theme-icon-dark" />
    </button>
  );
}
