// interAgentMessage.js 的单元测试(add-mesh-swarm-mode)。
//
// agent 间信封以 user 角色落盘,界面上必须显示成系统提示行,否则会开出假回合。
// 覆盖:
//  - 元数据解析与正文 Payload 提取
//  - 三种类型 / errored 状态各自的提示代码
//  - 普通 user 消息与非 user 消息原样返回
//  - 提示行经 presentSystemNotice 得到带发送方的标题与正文

import assert from 'node:assert/strict';
import {
  interAgentEnvelope,
  interAgentNoticeCode,
  interAgentPayload,
  presentInterAgentMessage,
} from './interAgentMessage.js';
import { presentSystemNotice } from './systemNotice.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function envelopeMessage(type, payload, status = '') {
  return {
    role: 'user',
    id: 'u-1',
    content: `<inter_agent_message>\nMessage Type: ${type}\nTask name: /root\nSender: /root/worker\nPayload:\n${payload}\n</inter_agent_message>`,
    metadata: {
      inter_agent: {
        type, sender: '/root/worker', recipient: '/root', sender_session_id: 's-w',
        ...(status ? { status } : {}),
      },
    },
  };
}

// 场景:daemon 落盘的信封(正文 + metadata.inter_agent)。
// 期望:解析出类型与路径;Payload 去掉头部与闭合标签;Payload 正文里再出现
// "Payload:" 也不截错;非信封文本原样返回。
run('解析信封元数据并提取 Payload 正文', () => {
  const message = envelopeMessage('MESSAGE', 'line 1\nPayload:\nline 2');
  assert.deepEqual(interAgentEnvelope(message.metadata), {
    type: 'MESSAGE', sender: '/root/worker', recipient: '/root', status: '', senderSessionId: 's-w',
  });
  assert.equal(interAgentPayload(message.content), 'line 1\nPayload:\nline 2');
  assert.equal(interAgentPayload('plain text'), 'plain text');
  assert.equal(interAgentEnvelope({ inter_agent: { type: 'OTHER' } }), null);
  assert.equal(interAgentEnvelope(null), null);
});

// 场景:四种提示 —— 派任务 / 发消息 / 完成 / 出错。
// 期望:FINAL_ANSWER 按 status 区分完成与出错,其余按类型。
run('按类型与状态选择提示代码', () => {
  assert.equal(interAgentNoticeCode(interAgentEnvelope(envelopeMessage('NEW_TASK', 'x').metadata)), 'inter_agent_task');
  assert.equal(interAgentNoticeCode(interAgentEnvelope(envelopeMessage('MESSAGE', 'x').metadata)), 'inter_agent_message');
  assert.equal(interAgentNoticeCode(interAgentEnvelope(envelopeMessage('FINAL_ANSWER', 'x', 'completed').metadata)), 'inter_agent_final');
  assert.equal(interAgentNoticeCode(interAgentEnvelope(envelopeMessage('FINAL_ANSWER', 'x', 'errored').metadata)), 'inter_agent_error');
});

// 场景:transcript 摄入一条信封、一条普通用户消息、一条 assistant 消息。
// 期望:只有信封被转成 system 行(保留 id 用于去重),正文换成 Payload;
// 其它消息引用不变。回归:若保持 user 角色,父 agent 的会话里每条子 agent
// 回报都会变成一个用户气泡并开出新回合。
run('只有信封被转成系统提示行', () => {
  const converted = presentInterAgentMessage(envelopeMessage('FINAL_ANSWER', 'report done', 'completed'));
  assert.equal(converted.role, 'system');
  assert.equal(converted.id, 'u-1');
  assert.equal(converted.content, 'report done');
  assert.equal(converted.metadata.system_notice.code, 'inter_agent_final');
  assert.equal(converted.metadata.system_notice.params.sender, '/root/worker');
  const ordinary = { role: 'user', content: 'hi', metadata: {} };
  assert.equal(presentInterAgentMessage(ordinary), ordinary);
  const assistant = { role: 'assistant', content: 'ok', metadata: { inter_agent: { type: 'MESSAGE' } } };
  assert.equal(presentInterAgentMessage(assistant), assistant);
});

// 场景:提示行交给 SystemRow 渲染。
// 期望:标题带上发送方路径(中文目录),展开正文就是 Payload。
run('提示行标题带发送方,正文为 Payload', () => {
  const converted = presentInterAgentMessage(envelopeMessage('FINAL_ANSWER', 'report done', 'completed'));
  const t = (key, options = {}) => {
    const templates = {
      'systemNotice.titles.inter_agent_final': '{{sender}} 已完成',
    };
    const template = templates[key];
    if (template == null) return options.defaultValue ?? key;
    return template.replace(/\{\{(\w+)\}\}/g, (_, name) => String(options[name] ?? ''));
  };
  const presented = presentSystemNotice(converted, t);
  assert.equal(presented.title, '/root/worker 已完成');
  assert.equal(presented.text, 'report done');
});
