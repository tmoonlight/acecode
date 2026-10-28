/** Trajectory glyph names remain stable while sharing the interface icon family. */
import { VsIcon } from '../../Icon.jsx'

interface IconProps {
  size?: number | undefined
  className?: string | undefined
}

export const IconSearchOutline16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="search" size={size} className={className} />
)

export const IconCheckOutline16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="check" size={size} className={className} />
)

export const IconChevronRightOutline14 = ({ size = 14, className }: IconProps) => (
  <VsIcon name="expandRight" size={size} className={className} />
)

export const IconCopyOutline16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="copy" size={size} className={className} />
)

export const IconSettingsOutline16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="settings" size={size} className={className} />
)

export const IconUserOutline16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="User" size={size} className={className} />
)

export const IconSparkle16 = ({ size = 16, className }: IconProps) => (
  <VsIcon name="Sparkle" size={size} className={className} />
)

export const IconCheckOutlineRegular = IconCheckOutline16
export const IconChevronRightOutlineRegular = IconChevronRightOutline14
export const IconCopyOutlineRegular = IconCopyOutline16
export const IconSettingsOutlineRegular = IconSettingsOutline16
export const IconUserOutlineRegular = IconUserOutline16
export const IconSparkleRegular = IconSparkle16
export const IconCodeOutlineRegular = ({ size = 16, className }: IconProps) => (
  <VsIcon name="code" size={size} className={className} />
)
export const IconWrapLinesOutlineRegular = ({ size = 16, className }: IconProps) => (
  <VsIcon name="wordWrap" size={size} className={className} />
)
