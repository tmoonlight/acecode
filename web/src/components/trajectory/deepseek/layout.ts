/** Minimal trajectory layout contracts used by the transplanted DeepSeek UI. */

import type { TrajectoryCellProps } from './trajectory-record.ts'

/** One Message or Step group inside a turn. */
export interface TrajectoryGroupModel {
  title: string
  description?: string
  cells: readonly TrajectoryCellProps[]
}

/** One sticky turn section. */
export interface TrajectoryTurnModel {
  turn: number | null
  groups: readonly TrajectoryGroupModel[]
}
