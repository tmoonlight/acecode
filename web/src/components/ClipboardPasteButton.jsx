import { clipboardPasteButtonAvailable, readTextFromSystemClipboard } from '../lib/systemClipboard.js';
import { toast } from './Toast.jsx';

// 密钥输入框旁的「粘贴」。只在安卓 App 里出现:那里密码框会弹出禁止粘贴的安全键盘,
// 网页自己也读不了剪贴板,只能经 App 的剪贴板通道读。粘进来的内容去掉首尾空白。
export function ClipboardPasteButton({ onPaste, className = '', disabled = false }) {
  if (!clipboardPasteButtonAvailable()) return null;
  const paste = async () => {
    const result = await readTextFromSystemClipboard();
    const text = result.ok ? result.text.trim() : '';
    if (!text) {
      toast({ kind: 'err', text: '剪贴板里没有文字' });
      return;
    }
    onPaste(text);
  };
  return (
    <button type="button" onClick={paste} disabled={disabled} className={className}>
      粘贴
    </button>
  );
}
