import {
  composerContentFromText, isPasteBlockPart, normalizeComposerContent,
} from './composerContent.js';
import { isComposerImageAttachment } from './richComposerModel.js';

// Raster images are shown as thumbnails and travel as snapshot uploads. SVG is
// deliberately excluded: it stays an ordinary file/path reference.
export function isRasterImageMimeType(mimeType) {
  return /^image\/(?:png|jpe?g|webp|gif|bmp|tiff|avif|heic|heif)$/i.test(String(mimeType || '').trim());
}

// Local raster references remain file resources in the transport, but have the
// same thumbnail presentation as clipboard image snapshots.
export function isComposerThumbnailAttachment(item) {
  return isComposerImageAttachment(item) || isRasterImageMimeType(item?.mime_type || item?.mimeType);
}

const isImagePart = (part) => part.type === 'attachment' && isComposerThumbnailAttachment(part);

// Editor projection: thumbnail images (shown in the strip above the input) and
// both kinds of pasted-text blocks (shown as cards) never enter Slate.
export function composerContentWithoutImages(value) {
  const content = normalizeComposerContent(value);
  return content ? normalizeComposerContent({
    ...content,
    parts: content.parts.filter((part) => !isImagePart(part) && !isPasteBlockPart(part)),
  }) : null;
}

// Merge order: images + editor content + paste blocks from `imageContent`
// (the full document) in their original order.
export function withComposerImageAttachments(editorContent, imageContent) {
  const content = composerContentWithoutImages(editorContent) || composerContentFromText('');
  const source = normalizeComposerContent(imageContent)?.parts || [];
  const seen = new Set();
  const images = source.filter((part) => {
    if (!isImagePart(part) || isPasteBlockPart(part)) return false;
    const identity = part.id || part.key;
    if (seen.has(identity)) return false;
    seen.add(identity);
    return true;
  });
  const pastes = source.filter(isPasteBlockPart);
  return normalizeComposerContent({ ...content, parts: [...images, ...content.parts, ...pastes] });
}

export function appendComposerImageAttachments(content, resources) {
  const current = normalizeComposerContent(content) || composerContentFromText('');
  const images = resources.filter(isComposerThumbnailAttachment).flatMap((item) => (
    composerContentFromText('', [item]).parts.map((part) => (
      // Do not turn a trusted path reference into a snapshot/image upload.
      item.kind === 'file' ? { ...part, kind: 'file' } : part
    ))
  ));
  return withComposerImageAttachments(current, { ...current, parts: [...current.parts, ...images] });
}

// Pasted-text file blocks: staged resources carrying a `paste` descriptor become
// attachment parts appended after everything else (blocks follow editor content).
export function appendPasteFileAttachments(content, resources = []) {
  const current = normalizeComposerContent(content) || composerContentFromText('');
  const known = new Set(current.parts.filter((part) => part.type === 'attachment')
    .flatMap((part) => [part.key, part.id].filter(Boolean)));
  const additions = [];
  for (const item of Array.from(resources || [])) {
    if (!item?.paste || typeof item.paste !== 'object') continue;
    const [part] = composerContentFromText('', [item]).parts;
    if (!part || known.has(part.key) || (part.id && known.has(part.id))) continue;
    known.add(part.key);
    if (part.id) known.add(part.id);
    additions.push({ ...part, kind: 'file' });
  }
  if (!additions.length) return current;
  return normalizeComposerContent({ ...current, parts: [...current.parts, ...additions] });
}
