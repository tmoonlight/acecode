import { useTranslation } from 'react-i18next';
import { SessionContentLoading } from './SessionContentLoading.jsx';

// 新建会话 / 首条消息里建 worktree 迟迟没完成时的等待遮罩。
// 什么时候出现由 useSessionStartMask 决定(等待够久才出现),所以这里不再延迟;
// 它盖住整个会话工作区,提示居中在对话列上,左侧会话列表不受影响。
export function SessionStartLoading({ kind = '', sessionId = '' }) {
  const { t } = useTranslation();
  return (
    <SessionContentLoading
      phase={kind ? 'starting' : ''}
      anchorKey={sessionId}
      label={kind === 'worktree'
        ? t('sessionNavigation.worktreeCreating')
        : t('sessionNavigation.sessionCreating')}
      revealDelayMs={0}
      trapFocus
      anchorSelector="[data-session-content-loading-anchor='true']"
    />
  );
}
