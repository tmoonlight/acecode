import { attachmentsFromContentParts, isImageAttachment } from './messageAttachments.js';

const HEADER = 'Page annotations\n';

export function parsePageAnnotationMessage(text) {
  const source = String(text ?? '');
  if (!source.startsWith(HEADER)) return null;
  let payload;
  try {
    payload = JSON.parse(source.slice(HEADER.length));
  } catch {
    return null;
  }
  if (!payload || typeof payload !== 'object' || Array.isArray(payload) || !Array.isArray(payload.annotations)) {
    return null;
  }
  const title = String(payload.page?.title || '').trim();
  const comments = [];
  payload.annotations.forEach((item, index) => {
    if (!item || typeof item !== 'object') return;
    const description = String(item.description || '').trim();
    if (!description) return;
    const number = Number(item.number);
    comments.push({
      number: Number.isInteger(number) && number > 0 ? number : index + 1,
      description,
    });
  });
  return { title, comments };
}

export function pageAnnotationMessageCopyText(card) {
  if (!card) return '';
  const lines = [];
  if (card.title) lines.push(card.title);
  const several = card.comments.length > 1;
  for (const comment of card.comments) {
    lines.push(several ? `${comment.number} ${comment.description}` : comment.description);
  }
  return lines.join('\n');
}

export function pageAnnotationScreenshot(contentParts) {
  return attachmentsFromContentParts(contentParts).find((attachment) => (
    isImageAttachment(attachment) &&
    /^page-annotations-[A-Za-z0-9_-]+\.png$/i.test(String(attachment.name || ''))
  )) || null;
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
