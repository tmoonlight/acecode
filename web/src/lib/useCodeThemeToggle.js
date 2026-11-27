import { useEffect, useRef } from 'react';
import { isCodeThemeToggleTarget } from './codeTheme.js';

// 代码块里的明暗按钮是 markdown 渲染出来的静态 HTML(聊天、侧边对话、文件里的
// markdown 预览都有),文件预览顶栏的按钮也带同一个 data 属性;统一在 document 的
// 捕获阶段接住,不在每个复制按钮的委托点各写一份。
export function useCodeThemeToggle(onToggle) {
  const handlerRef = useRef(onToggle);
  handlerRef.current = onToggle;
  useEffect(() => {
    const onClick = (event) => {
      if (!isCodeThemeToggleTarget(event.target)) return;
      event.preventDefault();
      handlerRef.current?.();
    };
    document.addEventListener('click', onClick, true);
    return () => document.removeEventListener('click', onClick, true);
  }, []);
}
