import hljs from 'highlight.js/lib/common';
import cmake from 'highlight.js/lib/languages/cmake';
import dockerfile from 'highlight.js/lib/languages/dockerfile';
import powershell from 'highlight.js/lib/languages/powershell';
import { langForFile } from './lang.js';

if (!hljs.getLanguage('cmake')) hljs.registerLanguage('cmake', cmake);
if (!hljs.getLanguage('dockerfile')) hljs.registerLanguage('dockerfile', dockerfile);
if (!hljs.getLanguage('powershell')) hljs.registerLanguage('powershell', powershell);

function escapeHtml(source) {
  return String(source)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}

export function highlightSourceForFile(path, source) {
  const text = String(source ?? '');
  const language = langForFile(path);
  // 不用 highlightAuto：纯文本和未知后缀必须保持无着色，不能猜错语言。
  if (!language || !hljs.getLanguage(language)) {
    return { language: '', html: escapeHtml(text) };
  }

  try {
    return {
      language,
      html: hljs.highlight(text, { language, ignoreIllegals: true }).value,
    };
  } catch {
    return { language: '', html: escapeHtml(text) };
  }
}
