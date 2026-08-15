import { JsonTree } from './JsonTree.tsx'
import { MarkdownText as DeepSeekMarkdownText } from './markdown/MarkdownText.tsx'
import { Tooltip } from './Tooltip.tsx'

export { JsonTree, Tooltip }
export {
  IconChevronRightOutline14,
  IconSettingsOutline16,
  IconSparkle16,
  IconUserOutline16,
} from './TrajectoryIcons.tsx'

const ENGLISH_CODE_LABELS = Object.freeze({
  copyLabel: 'Copy',
  copiedLabel: 'Copied',
})

/** DeepSeek's MarkdownText primitive with the requested English-only labels. */
export function MarkdownText({ text }: { text: string }) {
  return <DeepSeekMarkdownText text={text} codeLabels={ENGLISH_CODE_LABELS} />
}
