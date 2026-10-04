import { homeRefFromWorkspace } from './homeWorkspaceSelection.js';

export function scheduledTaskCreationRef(current, health) {
  return {
    ...homeRefFromWorkspace(current, current, health),
    composerDraftScope: 'scheduled-task',
    initialDraftText: '/scheduled-task 我希望在明天X点提醒我参加会议，重复X天',
  };
}
