import type { TrajectorySourceBlock } from './trajectory-record.ts'

/** Display metadata only; the original fields remain in the Raw block. */
export interface TrajectoryAttachment {
  name?: string
  bytes?: number
  mediaType?: string
  width?: number
  height?: number
  src?: string
}

export function safeTrajectoryImageSource(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined
  if (/^data:image\//i.test(value)
    || value.startsWith('blob:')) return value
  if (/^\/api\/sessions\/[^/?#]+\/attachments\/[^/?#]+\/blob(?:\?[^#]*)?$/.test(value)) return value
  try {
    const protocol = new URL(value).protocol
    return protocol === 'http:' || protocol === 'https:' ? value : undefined
  } catch {
    return undefined
  }
}

function finiteMetadata(value: unknown): number | undefined {
  if (value === null || value === undefined || value === '') return undefined
  const number = Number(value)
  return Number.isFinite(number) && number >= 0 ? number : undefined
}

export function trajectorySourceBlock(value: unknown): TrajectorySourceBlock {
  if (value === null || typeof value !== 'object') {
    return { type: 'text', content: typeof value === 'string' ? value : JSON.stringify(value) ?? '' }
  }
  const block = value as Record<string, unknown>
  const type = typeof block.type === 'string' ? block.type : 'unknown'
  if (typeof block.text === 'string') {
    return { type: type === 'reasoning' ? 'thinking' : type, content: block.text }
  }
  const ref = block.attachment !== null && typeof block.attachment === 'object'
    ? block.attachment as Record<string, unknown> : block
  const image = type.toLowerCase().includes('image')
  if (!image && type !== 'file') return { type, content: JSON.stringify(value, null, 2) }
  const source = block.source !== null && typeof block.source === 'object'
    ? block.source as Record<string, unknown> : {}
  const imageUrl = typeof block.image_url === 'object' && block.image_url !== null
    ? (block.image_url as Record<string, unknown>).url : block.image_url
  const mediaType = [ref.mime_type, ref.mimeType, ref.media_type, ref.mediaType, source.media_type]
    .find((item): item is string => typeof item === 'string')
  const data = typeof ref.data === 'string' ? ref.data : source.data
  const src = image ? [ref.blob_url, ref.preview_url, ref.url, imageUrl, source.url,
    typeof data === 'string' ? data.startsWith('data:') ? data : `data:${mediaType ?? 'image/png'};base64,${data}` : undefined,
  ].map(safeTrajectoryImageSource).find(item => item !== undefined) : undefined
  const name = typeof ref.name === 'string' ? ref.name.split(/[\\/]/).pop() : undefined
  const attachment: TrajectoryAttachment = {
    ...(name ? { name } : {}),
    bytes: finiteMetadata(ref.size ?? ref.bytes),
    mediaType,
    width: finiteMetadata(ref.width),
    height: finiteMetadata(ref.height),
    ...(src === undefined ? {} : { src }),
  }
  return {
    type: image ? 'image' : 'file',
    content: JSON.stringify(value, null, 2),
    ...(image ? { attachment, imageSrc: src } : { file: attachment }),
  }
}
