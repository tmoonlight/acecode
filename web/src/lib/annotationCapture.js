// 保存批注时补图（D2 / D10）。
//
// 捕获一次视口，按目标外接矩形裁出缩略图（宽 ≤512px）给待发卡片，同时把
// 整屏图存成该页的最新配图给已发消息卡片。发送阶段一张图都不再截。
//
// 坐标：targets[].bounds 来自 getBoundingClientRect，与 viewport 同一套 CSS 像素。
// 捕获图可能是 DPR 2 的物理像素，所以先按 viewport 尺寸重采样到 CSS 空间，再裁剪——
// 这样既修正了 DPR 换算，也把整屏图压回可传输的大小（T0 实测：不做修正时
// 2560×1440@DPR2 的 5120×2880 PNG 有 4.6MB，P95 238ms）。

const THUMBNAIL_MAX_WIDTH = 512;
const PADDING = 8;

export function captureRectForRecord(record, viewport) {
  const width = Number(viewport?.width);
  const height = Number(viewport?.height);
  if (!(width > 0) || !(height > 0)) return null;
  const boxes = [];
  if (record?.region && typeof record.region === 'object') boxes.push(record.region);
  for (const target of Array.isArray(record?.targets) ? record.targets : []) {
    if (target?.bounds) boxes.push(target.bounds);
  }
  let left = Infinity;
  let top = Infinity;
  let right = -Infinity;
  let bottom = -Infinity;
  for (const box of boxes) {
    if (!Number.isFinite(box?.x) || !Number.isFinite(box?.y) ||
        !(box?.width > 0) || !(box?.height > 0)) continue;
    left = Math.min(left, box.x - PADDING);
    top = Math.min(top, box.y - PADDING);
    right = Math.max(right, box.x + box.width + PADDING);
    bottom = Math.max(bottom, box.y + box.height + PADDING);
  }
  if (!(right > left) || !(bottom > top)) return null;
  const x = Math.max(0, Math.floor(left));
  const y = Math.max(0, Math.floor(top));
  const clippedWidth = Math.min(Math.ceil(right), Math.floor(width)) - x;
  const clippedHeight = Math.min(Math.ceil(bottom), Math.floor(height)) - y;
  if (clippedWidth <= 0 || clippedHeight <= 0) return null;
  return { x, y, width: clippedWidth, height: clippedHeight };
}

function loadImage(source) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error('截图解码失败'));
    image.src = source;
  });
}

function canvasContext(width, height) {
  const canvas = document.createElement('canvas');
  canvas.width = Math.max(1, Math.round(width));
  canvas.height = Math.max(1, Math.round(height));
  return { canvas, context: canvas.getContext('2d') };
}

// 捕获图 → CSS 空间画布（顺便完成 scale = 1/dpr 的重采样）。
function normalizeCapture(image, viewport) {
  const { canvas, context } = canvasContext(viewport.width, viewport.height);
  context.imageSmoothingQuality = 'high';
  context.drawImage(image, 0, 0, canvas.width, canvas.height);
  return canvas;
}

function cropThumbnail(source, rect) {
  const scale = Math.min(1, THUMBNAIL_MAX_WIDTH / rect.width);
  const { canvas, context } = canvasContext(rect.width * scale, rect.height * scale);
  context.imageSmoothingQuality = 'high';
  context.drawImage(source, rect.x, rect.y, rect.width, rect.height, 0, 0, canvas.width, canvas.height);
  return canvas.toDataURL('image/png');
}

export async function cropAnnotationImages(base64, rect, viewport) {
  if (!rect) return { thumbnail: '', pageShot: '' };
  const image = await loadImage(`data:image/png;base64,${base64}`);
  const page = normalizeCapture(image, viewport);
  return {
    thumbnail: cropThumbnail(page, rect),
    pageShot: page.toDataURL('image/png'),
  };
}
