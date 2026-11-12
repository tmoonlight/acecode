import '../../../assets/desktop_pet/office_characters.js';

export const { stableSeed, stableLook, characterSprite, miniSprite } = globalThis.ACEOfficeCharacters;

export function expertAvatarIdentity(expert = {}) {
  if (expert.avatar_identity) return String(expert.avatar_identity);
  const id = String(expert.id || '');
  if (!id) return '';
  if (expert.references_existing_experts) return `expert:${expert.lead_expert_id || id}`;
  if (expert.type === 'team' && expert.lead_agent_id) return `expert:${id}/${expert.lead_agent_id}`;
  return `expert:${id}`;
}

// 子代理的头像:绑定了专家就是那位专家的;否则按子会话 id 生成一个固定的像素小人,
// 同一个子代理在对话、摘要面板、后台任务页签里是同一张脸。
export function subagentAvatarIdentity(sessionId, expert = null) {
  if (expert?.avatar_identity) return String(expert.avatar_identity);
  const id = String(sessionId || '');
  return id ? `subagent:${id}` : '';
}

export function sessionAvatarIdentity(session = {}) {
  if (session.expert?.avatar_identity) return String(session.expert.avatar_identity);
  const id = String(session.expert_id || session.expert?.id || '');
  const member = String(session.expert_member_id || session.expert?.member_id || '');
  return id ? `expert:${id}${member ? `/${member}` : ''}` : '';
}
