export const DESKTOP_PET_KINDS = Object.freeze([
  { id: 'office', label: '虚拟办公室', description: '在像素办公室里查看 Agent 协作。' },
  { id: 'clippy', label: '大眼回形针', description: '熟悉的回形针，陪你思考和工作。' },
  { id: 'rover', label: 'rover', description: '经典小狗，随任务状态做出反应。' },
  { id: 'whale', label: '鲸鱼娘', description: '蓝发鲸鱼娘，工作和休息都陪着你。' },
]);

export function normalizeDesktopPetKind(value) {
  return DESKTOP_PET_KINDS.some(pet => pet.id === value) ? value : 'office';
}
