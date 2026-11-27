// skill_view 的结果是写给模型的 JSON,content 里是 SKILL.md 正文或技能附属文件的内容。
// 用户展开这一行时要看的是文档本身:markdown 照常渲染,脚本等附属文件按源码高亮。
// 认不出来的结果(失败、被截断、旧会话压缩后的占位文字)一律返回 null,调用方照旧显示原文。

const SKILL_DOCUMENT_TOOLS = new Set(['skill_view']);
const MARKDOWN_FILE = /\.(?:md|markdown|mdx)$/i;
// 读不到去掉 frontmatter 的正文时,daemon 会退回整份 SKILL.md;开头那段
// `---` 包起来的元数据直接渲染会变成一条横线加一个大标题,所以先剥掉。
// 只认第一行是 `key:` 的元数据段,以分隔线开头的普通正文不能被误删。
const LEADING_FRONTMATTER = /^﻿?---[ \t]*\n(?=[A-Za-z_][\w-]*[ \t]*:)[\s\S]*?\n---[ \t]*(?:\n|$)/;

function parseObject(text) {
  if (!text.startsWith('{')) return null;
  try {
    const value = JSON.parse(text);
    return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
  } catch {
    return null;
  }
}

export function skillViewDocument(tool, output) {
  if (!SKILL_DOCUMENT_TOOLS.has(String(tool || ''))) return null;
  const parsed = parseObject(typeof output === 'string' ? output.trim() : '');
  if (!parsed || parsed.success !== true || typeof parsed.content !== 'string') return null;
  const name = typeof parsed.name === 'string' ? parsed.name : '';
  const filePath = typeof parsed.file_path === 'string' ? parsed.file_path.trim() : '';
  const content = parsed.content.replace(/\r\n?/g, '\n');
  if (filePath && !MARKDOWN_FILE.test(filePath)) {
    return content.trim() ? { kind: 'source', name, path: filePath, content } : null;
  }
  const markdown = content.replace(LEADING_FRONTMATTER, '');
  if (!markdown.trim()) return null;
  return { kind: 'markdown', name, path: filePath || 'SKILL.md', content: markdown };
}

// 从 /api/skills 的清单里按技能名取 SKILL.md 的绝对路径;找不到返回空串。
// 打开文件夹时传 SKILL.md 本身:资源管理器会打开技能目录并选中它。
// 附属文件也一样传 SKILL.md,不在前端拿 file_path 拼路径。
export function skillFilePathFromCatalog(skills, name) {
  const wanted = String(name || '').trim();
  if (!wanted || !Array.isArray(skills)) return '';
  const skill = skills.find((item) => item && item.name === wanted);
  const path = typeof skill?.path === 'string' ? skill.path.trim() : '';
  return path;
}
