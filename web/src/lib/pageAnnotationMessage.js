import {
  attachmentsFromContentParts, contextsFromContentParts, isImageAttachment,
} from './messageAttachments.js';

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

function pageAnnotationCard(rawPages) {
  const pages = (Array.isArray(rawPages) ? rawPages : [])
    .map(normalizePage)
    .filter((page) => page.comments.length > 0);
  if (!pages.length) return null;
  const comments = pages.flatMap((page) => page.comments);
  return {
    title: pages.length === 1 ? pages[0].title : '',
    pages,
    comments,
  };
}

export function pageAnnotationCardFromContexts(contentParts) {
  const context = contextsFromContentParts(contentParts)
    .find((item) => item.type === 'page_annotations');
  if (!context) return null;
  const card = pageAnnotationCard(context.pages);
  return card ? { text: '', ...card } : null;
}

// 复制出去的是人读文本，不是原始载荷（PRD §6.7）。
export function pageAnnotationMessageCopyText(card, text = card?.text) {
  if (!card) return '';
  const lines = [];
  if (text) lines.push(text, '');
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
