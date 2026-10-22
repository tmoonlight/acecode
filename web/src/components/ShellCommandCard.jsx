import { useLayoutEffect, useRef, useState } from 'react';
import { clsx } from '../lib/format.js';
import { SHELL_PREVIEW_LINES } from '../lib/shellCommandPresentation.js';
import { CopyableCodeFrame } from './CopyableCodeFrame.jsx';
import { VsIcon } from './Icon.jsx';
import '../styles/shell-command-card.css';

export function ShellCommandCard({ presentation, expanded, onToggle, running, failed, status, contextAttrs, transcript = false, showCommand = true }) {
  const { command, output, copyText } = presentation;
  const commandRef = useRef(null);
  const outputRef = useRef(null);
  const [clipped, setClipped] = useState({ command: false, output: false });
  useLayoutEffect(() => {
    const overflows = (el) => !!el && el.scrollHeight > parseFloat(getComputedStyle(el).lineHeight) * SHELL_PREVIEW_LINES + 1;
    const measure = () => {
      const next = { command: overflows(commandRef.current), output: overflows(outputRef.current) };
      setClipped((prev) => prev.command === next.command && prev.output === next.output ? prev : next);
    };
    measure();
    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', measure);
      return () => window.removeEventListener('resize', measure);
    }
    const observer = new ResizeObserver(measure);
    if (commandRef.current) observer.observe(commandRef.current);
    if (outputRef.current) observer.observe(outputRef.current);
    return () => observer.disconnect();
  }, [command, output]);

  return (
    <div
      {...contextAttrs}
      className="ace-shell-command-card"
      data-shell-command-card="true"
      data-expanded={expanded ? 'true' : 'false'}
      data-transcript={transcript ? 'true' : undefined}
      style={{ '--ace-shell-preview-lines': SHELL_PREVIEW_LINES }}
    >
      {!transcript && <button
        type="button"
        className="ace-shell-card-header"
        onClick={onToggle}
        aria-expanded={expanded}
        aria-label={expanded ? '收起命令详情' : '展开命令详情'}
      >
        <span>Shell</span>
        <span className="ace-shell-card-status">
          {running && <span className="ace-spinner h-3 w-3 shrink-0" />}
          {failed && <span className="text-danger">执行失败</span>}
          {status}
        </span>
        <VsIcon name="expandDown" size={14} className="ace-shell-card-chevron" style={{ transform: `rotate(${expanded ? 180 : 0}deg)` }} />
      </button>}
      <CopyableCodeFrame text={copyText} className="ace-shell-card-body">
        {transcript && status}
        {showCommand && <div className={clsx('ace-shell-card-command', !expanded && clipped.command && 'is-clipped')}>
          <pre ref={commandRef} className="ace-shell-card-code">
            {command ? <><span className="text-fg-mute">$ </span>{command}</> : <span className="text-fg-mute">命令未记录</span>}
          </pre>
        </div>}
        {output && (
          <div className={clsx('ace-shell-card-output', !expanded && clipped.output && 'is-clipped')}>
            <pre ref={outputRef} className="ace-shell-card-code">{output}</pre>
          </div>
        )}
      </CopyableCodeFrame>
    </div>
  );
}
