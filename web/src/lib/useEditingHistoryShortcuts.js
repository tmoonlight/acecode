import { useEffect } from 'react';
import { handleNativeHistoryKey } from './editingHistoryShortcuts.js';

export function useEditingHistoryShortcuts() {
  useEffect(() => {
    let composing = false;
    let timer;
    const start = () => { clearTimeout(timer); composing = true; };
    const end = () => { timer = setTimeout(() => { composing = false; }, 0); };
    const key = (event) => { if (!composing) handleNativeHistoryKey(event); };
    window.addEventListener('compositionstart', start, true);
    window.addEventListener('compositionend', end, true);
    window.addEventListener('keydown', key);
    return () => {
      clearTimeout(timer);
      window.removeEventListener('compositionstart', start, true);
      window.removeEventListener('compositionend', end, true);
      window.removeEventListener('keydown', key);
    };
  }, []);
}
