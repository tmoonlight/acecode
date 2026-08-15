import { useMemo } from 'react'

import { renderMarkdown } from '../../../lib/markdown.js'
import { JsonTree } from './JsonTree.tsx'
import { Tooltip } from './Tooltip.tsx'

export { JsonTree, Tooltip }
export {
  IconChevronRightOutline14,
  IconSettingsOutline16,
  IconSparkle16,
  IconUserOutline16,
} from './TrajectoryIcons.tsx'

/** ACECode renderer adapter for DeepSeek's MarkdownText primitive. */
export function MarkdownText({ text }: { text: string }) {
  const html = useMemo(() => ({ __html: renderMarkdown(text) }), [text])
  return <div className="ace-trajectory-markdown" dangerouslySetInnerHTML={html} />
}
