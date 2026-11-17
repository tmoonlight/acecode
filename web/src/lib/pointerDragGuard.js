// 指针拖动的「主键已松开」守卫。
//
// 自绘拖动在 pointerdown 时进入待命,移动超过阈值才真正拖起来,松手靠 pointerup 收尾。
// macOS 触控板开了「轻点以点按」时,用户反馈轻点侧栏后行会「吸」在光标上跟着走;
// 疑似轻点的松开没有以 pointerup 送到这套收尾逻辑,手势一直待命,手指随后滑动光标就越过阈值。
// 这时浏览器在 pointermove 上报的 buttons 已经不含主键,据此判定这次按下早已结束,
// 应当取消拖动,而不是把不按键的移动当成拖动。真正按住拖动时 buttons 一定含主键,
// 所以这条判定不影响正常拖动。
//
// 只看鼠标:触控 / 手写笔的 buttons 语义不同(悬停、接触各有约定),这里不碰。
// buttons 不是数字(老旧环境、合成事件)时按「不确定」处理,不取消。
export function pointerDragButtonReleased(event) {
  if (!event || event.pointerType !== 'mouse') return false;
  if (typeof event.buttons !== 'number') return false;
  return (event.buttons & 1) === 0;
}
