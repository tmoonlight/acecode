import { useState } from 'react'
import type { ReactNode } from 'react'
import { FileTypeIcon } from '../../Icon.jsx'
import { ImageLightbox } from '../../ImageLightbox.jsx'
import { formatBytes } from '../../../lib/format.js'
import { JsonTree } from './JsonTree.tsx'
import { MarkdownText as DeepSeekMarkdownText } from './markdown/MarkdownText.tsx'
import { Tooltip } from './Tooltip.tsx'
import type { TrajectoryAttachment } from './trajectory-attachments.ts'
import { safeTrajectoryImageSource } from './trajectory-attachments.ts'
import css from './TrajectoryTable.module.css'

export { JsonTree, Tooltip, FileTypeIcon }
export { writeClipboard } from './clipboard.ts'
export { CodeBlock } from './markdown/CodeBlock.tsx'
export type { JsonTreeLabels, JsonTreeProps } from './JsonTree.tsx'
export type { ConversationPromptSnapshot } from './trajectory-record.ts'
export * from './TrajectoryIcons.tsx'

export interface AssistantRequestConfig {
  provider?: string
  model?: string
  [key: string]: unknown
}
export interface MarkdownLabels {
  code: { copyLabel: string; copiedLabel: string }
  footnotes: string
}
interface ImageOwner {
  images: readonly { attachment: TrajectoryAttachment; label: string }[]
  align?: string
  thumbnail?: boolean
  loadImage?: MessageImageLoader
}
export type MessageImageLoader = (attachment: TrajectoryAttachment) => Promise<string>
export type PropsRenderSlots<_Slot extends string> = {
  renderSlot: (slot: string, owner: ImageOwner) => ReactNode
}

const ENGLISH_CODE_LABELS = Object.freeze({
  copyLabel: 'Copy',
  copiedLabel: 'Copied',
})

/** Keep the incumbent labels and math renderer while supporting compact thinking. */
export function MarkdownText({ text, labels, variant = 'body' }: {
  text: string; labels?: MarkdownLabels; variant?: 'body' | 'compact'
}) {
  return <div data-markdown-variant={variant} className={variant === 'compact' ? css.compactMarkdown : undefined}>
    <DeepSeekMarkdownText text={text} codeLabels={labels?.code ?? ENGLISH_CODE_LABELS} />
  </div>
}

export function fileExtension(name: string): string { return name.split('.').slice(1).pop() ?? '' }
export function fileSizeText(bytes?: number): string {
  return formatBytes(bytes)
}

export function StateDot({ state }: { state: string }) {
  return <span className={css.loadingDot} data-state={state} aria-hidden="true" />
}

function TrajectoryImages({ images }: ImageOwner) {
  const [preview, setPreview] = useState<{ src: string; alt: string } | null>(null)
  return <>
    {images.map(({ attachment, label }, index) => {
      const src = safeTrajectoryImageSource(attachment.src)
      return src === undefined ? <FileTypeIcon key={index} path={label} /> : (
        <button key={index} type="button" className={css.attachmentThumbnail} aria-label={label}
          onClick={() => setPreview({ src, alt: label })}>
          <img src={src} alt={label} loading="lazy" />
        </button>
      )
    })}
    {preview && <ImageLightbox preview={preview} onClose={() => setPreview(null)} />}
  </>
}

export function renderTrajectoryImages(_slot: string, owner: ImageOwner): ReactNode {
  return <TrajectoryImages {...owner} />
}
