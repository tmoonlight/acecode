import '../../../assets/desktop_pet/office_characters.js';

export const { stableSeed, stableLook, characterSprite } = globalThis.ACEOfficeCharacters;

export function expertAvatarIdentity(expert = {}) {
  if (expert.avatar_identity) return String(expert.avatar_identity);
  const id = String(expert.id || '');
  if (!id) return '';
  if (expert.references_existing_experts) return `expert:${expert.lead_expert_id || id}`;
  if (expert.type === 'team' && expert.lead_agent_id) return `expert:${id}/${expert.lead_agent_id}`;
  return `expert:${id}`;
}

export function sessionAvatarIdentity(session = {}) {
  if (session.expert?.avatar_identity) return String(session.expert.avatar_identity);
  const id = String(session.expert_id || session.expert?.id || '');
  const member = String(session.expert_member_id || session.expert?.member_id || '');
  return id ? `expert:${id}${member ? `/${member}` : ''}` : '';
}
