import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';

function parseDesktopResult(value) {
  if (value == null) return value;
  if (typeof value !== 'string') return value;
  const text = value.trim();
  if (!text || text === 'null') return null;
  return JSON.parse(text);
}

export async function openTargetInExplorer(openTarget) {
  if (!openTarget?.path) {
    toast({ kind: 'err', text: '无法打开:路径为空' });
    return;
  }
  try {
    let result;
    if (typeof window.aceDesktop_openInExplorer === 'function') {
      result = parseDesktopResult(await window.aceDesktop_openInExplorer(openTarget.path));
    } else {
      // webapp 兼容模式没有 webview bridge,daemon 就在本机,改走 REST 端点。
      result = await api.openInExplorer(openTarget.path);
    }
    if (!result?.ok) {
      toast({ kind: 'err', text: '打开失败:' + (result?.error || '') });
      return;
    }
    toast({ kind: 'ok', text: '已打开文件夹' });
  } catch (e) {
    const detail = e?.body?.error || e?.message || '';
    toast({ kind: 'err', text: '打开失败:' + detail });
  }
}
