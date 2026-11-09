// 后台任务(后台命令 / 后台子代理)的通知消息。
//
// daemon 把「后台任务完成 / 有新输出 / 长时间无输出」通知落盘成 user 角色的消息
// (正文是给模型看的 <background-task> 文本,metadata.background_task 带任务快照),
// 回合进行中在模型边界插入,空闲时唤醒新回合。界面上它们不是用户说的话,进
// transcript 之前统一转成系统提示行:标题写清是哪个任务、结果如何,展开看正文。

const OPEN_TAG = /^<background-task\b[^>]*>\s*\n?/;
const CLOSE_TAG = /\n?<\/background-task>\s*$/;

export function backgroundTaskNotice(metadata) {
  const value = metadata && typeof metadata === 'object' ? metadata.background_task : null;
  if (!value || typeof value !== 'object') return null;
  const kind = value.kind === 'agent' ? 'agent' : 'shell';
  return {
    id: String(value.id || ''),
    kind,
    event: String(value.event || 'finished'),
    state: String(value.state || ''),
    title: String(value.title || value.command || value.id || ''),
    command: String(value.command || ''),
    exitCode: Number.isFinite(Number(value.exit_code)) ? Number(value.exit_code) : null,
    outputFile: String(value.output_file || ''),
  };
}

export function backgroundTaskNoticeCode(notice) {
  if (!notice) return '';
  if (notice.kind === 'agent') {
    if (notice.state === 'failed') return 'background_agent_failed';
    if (notice.state === 'stopped') return 'background_agent_stopped';
    return 'background_agent_finished';
  }
  if (notice.event === 'output') return 'background_command_output';
  if (notice.event === 'quiet') return 'background_command_quiet';
  if (notice.state === 'failed') return 'background_command_failed';
  if (notice.state === 'stopped') return 'background_command_stopped';
  return 'background_command_finished';
}

// 去掉给模型看的 <background-task ...> 外壳,只留正文。
export function backgroundTaskBody(content) {
  return String(content ?? '').replace(OPEN_TAG, '').replace(CLOSE_TAG, '');
}

// user 角色的后台任务通知 → 系统提示行(system_notice);其它消息原样返回。
export function presentBackgroundTaskMessage(message) {
  if (!message || typeof message !== 'object' || message.role !== 'user') return message;
  const notice = backgroundTaskNotice(message.metadata);
  if (!notice) return message;
  const body = backgroundTaskBody(message.content);
  return {
    ...message,
    role: 'system',
    content: body,
    metadata: {
      ...message.metadata,
      system_notice: {
        version: 1,
        code: backgroundTaskNoticeCode(notice),
        params: {
          task: notice.title,
          text: body,
        },
      },
    },
  };
}
