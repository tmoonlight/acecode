// 蜂群模式（网状）的 agent 间消息(add-mesh-swarm-mode)。
//
// daemon 把 agent 间信封落盘成 user 角色的消息(正文是给模型看的
// <inter_agent_message> 文本,metadata.inter_agent 带类型 / 发送方 / 接收方),
// 这样任何 provider 都不会遇到两条相邻的 assistant 消息。界面上它们不是用户说的话:
// 当成用户气泡会开出一个假回合(回合滚动条、吸顶条、「重发末尾用户消息」都会认它)。
// 所以进 transcript 之前统一转成系统提示行,标题写清是谁发来的,展开看正文。

const NOTICE_CODES = {
  NEW_TASK: 'inter_agent_task',
  MESSAGE: 'inter_agent_message',
};

export function interAgentEnvelope(metadata) {
  const value = metadata && typeof metadata === 'object' ? metadata.inter_agent : null;
  if (!value || typeof value !== 'object') return null;
  const type = String(value.type || '');
  if (type !== 'NEW_TASK' && type !== 'MESSAGE' && type !== 'FINAL_ANSWER') return null;
  return {
    type,
    sender: String(value.sender || ''),
    recipient: String(value.recipient || ''),
    status: String(value.status || ''),
    senderSessionId: String(value.sender_session_id || ''),
  };
}

// 信封正文里 "Payload:" 之后的部分(去掉结尾的闭合标签);不是信封时原样返回。
export function interAgentPayload(content) {
  const text = String(content ?? '');
  const marker = '\nPayload:\n';
  const start = text.indexOf(marker);
  if (start < 0) return text;
  let payload = text.slice(start + marker.length);
  const close = '\n</inter_agent_message>';
  if (payload.endsWith(close)) payload = payload.slice(0, -close.length);
  return payload;
}

export function interAgentNoticeCode(envelope) {
  if (!envelope) return '';
  if (envelope.type === 'FINAL_ANSWER') {
    return envelope.status === 'errored' ? 'inter_agent_error' : 'inter_agent_final';
  }
  return NOTICE_CODES[envelope.type] || '';
}

// user 角色的信封 → 系统提示行(system_notice);其它消息原样返回。
export function presentInterAgentMessage(message) {
  if (!message || typeof message !== 'object' || message.role !== 'user') return message;
  const envelope = interAgentEnvelope(message.metadata);
  if (!envelope) return message;
  const payload = interAgentPayload(message.content);
  return {
    ...message,
    role: 'system',
    content: payload,
    metadata: {
      ...message.metadata,
      system_notice: {
        version: 1,
        code: interAgentNoticeCode(envelope),
        params: {
          sender: envelope.sender,
          recipient: envelope.recipient,
          text: payload,
        },
      },
    },
  };
}
