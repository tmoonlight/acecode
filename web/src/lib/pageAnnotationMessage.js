import { attachmentsFromContentParts, isImageAttachment } from './messageAttachments.js';

const HEADER = 'Page annotations\n';
const SEPARATOR = '\n\nPage annotations\n';

function splitMessage(text) {
  const source = String(text ?? '');
  const index = source.lastIndexOf(HEADER);
  if (index < 0) return null;
  // 头部要么在开头（v1 纯批注消息），要么前面正好隔一个空行（v2 用户正文 + 批注）。
  // 中间随便出现的字符串不算，避免把用户正文里的同名字样当载荷。
  if (index !== 0 && source.slice(index - 2, index) !== '\n\n') return null;
  return {
    head: index === 0 ? '' : source.slice(0, index - 2),
    body: source.slice(index + HEADER.length),
  };
}

function normalizeAnnotation(item, index, pageTitle) {
  if (!item || typeof item !== 'object') return null;
  const description = String(item.description || '').trim();
  if (!description) return null;
  const number = Number(item.number);
  const tag = String(
    item.element || (Array.isArray(item.targets) ? item.targets[0]?.tag : '') || '',
  ).trim().toLowerCase();
  return {
    number: Number.isInteger(number) && number > 0 ? number : index + 1,
    element: tag,
    description,
    pageTitle: pageTitle || '',
    state: String(item.state || ''),
  };
}

function normalizePage(page, index) {
  const title = String(page?.title || '').trim();
  const annotations = Array.isArray(page?.annotations) ? page.annotations : [];
  const comments = [];
  annotations.forEach((item, position) => {
    const comment = normalizeAnnotation(item, position, title);
    if (comment) comments.push(comment);
  });
  return {
    index,
    title,
    url: String(page?.url || '').trim(),
    shot: String(page?.shot || ''),
    comments,
  };
}

// 兼容两种载荷：
//   v1 `Page annotations\n{page, annotations}`（旧消息，纯文本逐条）
//   v2 `{用户正文}\n\nPage annotations\n{version, pages:[{title,url,shot,annotations}]}`
export function parsePageAnnotationMessage(text) {
  const split = splitMessage(text);
  if (!split) return null;
  let payload;
  try {
    payload = JSON.parse(split.body);
  } catch {
    return null;
  }
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return null;
  let rawPages;
  if (Array.isArray(payload.pages)) rawPages = payload.pages;
  else if (Array.isArray(payload.annotations)) rawPages = [{ ...(payload.page || {}), annotations: payload.annotations }];
  else return null;
  const pages = rawPages.map(normalizePage).filter((page) => page.comments.length > 0);
  if (!pages.length) return null;
  const comments = pages.flatMap((page) => page.comments);
  return {
    text: split.head.trim(),
    title: pages.length === 1 ? pages[0].title : '',
    pages,
    comments,
  };
}

// 复制出去的是人读文本，不是原始载荷（PRD §6.7）。
export function pageAnnotationMessageCopyText(card) {
  if (!card) return '';
  const lines = [];
  if (card.text) lines.push(card.text, '');
  const multiPage = (card.pages || []).length > 1;
  const showNumbers = card.comments.length > 1;
  for (const page of card.pages || []) {
    if (multiPage && page.title) lines.push(page.title);
    for (const comment of page.comments) {
      const prefix = [showNumbers ? String(comment.number) : '', comment.element].filter(Boolean).join(' ');
      lines.push(prefix ? `${prefix} ${comment.description}` : comment.description);
    }
  }
  if (!card.pages?.length && card.title) lines.unshift(card.title);
  return lines.join('\n');
}

const SHOT_NAME = /^page-annotations-[A-Za-z0-9_-]+\.png$/i;

export function pageAnnotationScreenshots(contentParts) {
  return attachmentsFromContentParts(contentParts).filter((attachment) => (
    isImageAttachment(attachment) && SHOT_NAME.test(String(attachment.name || ''))
  ));
}

export function pageAnnotationScreenshot(contentParts) {
  return pageAnnotationScreenshots(contentParts)[0] || null;
}

export function annotationScreenshotUrl(attachment) {
  return attachment?.blob_url || attachment?.preview_url || attachment?.url || '';
}

// 按载荷里的 `shot` 文件名取图：多页面消息靠它把每张图对到正确的页面。
export function annotationShotsByName(contentParts) {
  const shots = new Map();
  for (const attachment of pageAnnotationScreenshots(contentParts)) {
    const name = String(attachment.name || '');
    if (!shots.has(name)) shots.set(name, attachment);
  }
  return shots;
}

export function contentPartsWithoutAnnotationScreenshot(contentParts, screenshot) {
  if (!screenshot) return contentParts;
  return (Array.isArray(contentParts) ? contentParts : []).filter((part) => {
    const attachment = part?.attachment;
    if (!attachment) return true;
    if (screenshot.id && attachment.id === screenshot.id) return false;
    return attachment.name !== screenshot.name;
  });
}

// 批注图全部从缩略图条里摘掉：它们由卡片自己渲染，重复显示会像多传了两张图。
export function contentPartsWithoutAnnotationScreenshots(contentParts) {
  return (Array.isArray(contentParts) ? contentParts : []).filter((part) => {
    const attachment = part?.attachment;
    return !attachment || !SHOT_NAME.test(String(attachment.name || ''));
  });
}
