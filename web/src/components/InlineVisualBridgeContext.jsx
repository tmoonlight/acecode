// 对话内可视化(```visual 代码块)隔离框的追问 / 链接桥(openspec add-inline-visuals)。
//
// ChatView(主会话)提供 send:会话空闲时把文本作为用户消息发出,忙时只填进输入框。
// 没有 Provider 的渲染路径(子会话面板等)追问不生效,链接照常交给系统浏览器。
import { createContext } from 'react';
import { openExternalUrl } from '../lib/externalUrl.js';

export const defaultInlineVisualBridge = Object.freeze({
  send: null,
  openLink: (url) => openExternalUrl(url),
});

export const InlineVisualBridgeContext = createContext(defaultInlineVisualBridge);
