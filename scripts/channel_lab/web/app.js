const $ = (id) => document.getElementById(id);
const histories = { self: new Map(), acecode: new Map() };
const drafts = { self: '', acecode: '' };
let room = 'self';
let connected = false;
let sending = false;
let stopped = false;
let lastRender = '';
let pollTimer;
const labels = { sending: '发送中', sent: '已发送', received: '已接收', failed: '发送失败', unknown: '送达未确认' };

async function api(path, data) {
  const response = await fetch(path, {
    method: data === undefined ? 'GET' : 'POST',
    headers: data === undefined ? {} : { 'Content-Type': 'application/json' },
    body: data === undefined ? undefined : JSON.stringify(data),
    signal: AbortSignal.timeout(30000),
  });
  const value = await response.json();
  if (!response.ok) throw new Error(value.error || `请求失败 (${response.status})`);
  return value;
}

function error(message = '') {
  $('error').textContent = message;
  $('error').hidden = !message;
}

function node(tag, className, text) {
  const element = document.createElement(tag);
  element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function scrollBottom() {
  $('messages').scrollTop = $('messages').scrollHeight;
  $('jump').hidden = true;
}

function render(force = false) {
  const messages = [...histories[room].values()].sort((a, b) => a.id - b.id);
  const signature = room + JSON.stringify(messages);
  if (!force && lastRender === signature) return;
  lastRender = signature;
  const panel = $('messages');
  const follow = panel.scrollHeight - panel.scrollTop - panel.clientHeight < 80;
  const fragment = document.createDocumentFragment();
  for (const message of messages) {
    const mine = message.sender === 'me';
    const item = node('article', `message${mine ? ' mine' : ''}${message.kind === 'tool_call' ? ' tool' : ''}`);
    item.dataset.id = message.id;
    const meta = node('div', 'message-meta');
    const sender = mine ? '我' : message.sender === 'echo' ? '自己 · 服务端回送' : message.sender === 'system' ? 'IM 帮助' : 'ACECode';
    meta.append(node('span', '', sender));
    const date = new Date(message.created * 1000);
    meta.append(node('time', '', date.toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' })));
    item.append(meta, node('div', 'bubble', message.text));
    const delivery = node('div', `delivery ${message.status}`, labels[message.status] || message.status);
    const copy = node('button', 'copy', '复制');
    copy.addEventListener('click', async () => {
      try { await navigator.clipboard.writeText(message.text); copy.textContent = '已复制'; }
      catch { error('复制失败，请选择消息文字手动复制。'); }
    });
    delivery.append(copy);
    if (message.status === 'failed' || message.status === 'unknown') {
      const retry = node('button', 'text-button', '放回输入框');
      retry.addEventListener('click', () => { $('message-input').value = message.text; $('message-input').focus(); });
      delivery.append(retry);
    }
    item.append(delivery);
    fragment.append(item);
  }
  $('message-list').replaceChildren(fragment);
  $('empty').hidden = messages.length > 0;
  $('older').hidden = messages.length < 200;
  if (follow || force) scrollBottom();
  else $('jump').hidden = false;
}

async function sync() {
  const requestedRoom = room;
  const state = await api(`/api/state?room=${requestedRoom}`);
  for (const message of state.messages) histories[requestedRoom].set(message.id, message);
  connected = state.connected;
  $('nav-status').textContent = connected ? 'Channel 已连接' : 'Channel 未连接';
  $('service-status').textContent = '本机服务在线';
  $('service-dot').classList.add('online');
  $('binding-state').textContent = connected ? 'Channel 已连接' : 'Channel 未连接';
  $('channel-info').classList.toggle('connected', connected);
  $('session-info').textContent = state.session_id ? `${state.session_title || '会话'} · ${state.session_id.slice(-9)}` : '等待绑定测试会话';
  $('session-info').title = state.session_id || '';
  $('connection').textContent = connected ? '断开连接' : '连接 ACECode';
  $('connection').disabled = !state.daemon_ready;
  if (state.error && room === 'acecode') error(state.error);
  if (requestedRoom === room) render();
}

async function poll() {
  if (stopped) return;
  try { await sync(); }
  catch {
    $('service-status').textContent = '服务连接中断';
    $('service-dot').classList.remove('online');
    $('binding-state').textContent = '服务连接中断';
    $('nav-status').textContent = '连接状态未知';
    $('channel-info').classList.remove('connected');
    $('connection').textContent = '连接 ACECode';
    connected = false;
  }
  pollTimer = setTimeout(poll, 1000);
}

function selectRoom(next) {
  drafts[room] = $('message-input').value;
  room = next;
  $('message-input').value = drafts[room];
  for (const button of document.querySelectorAll('[data-room]')) {
    button.classList.toggle('selected', button.dataset.room === room);
    if (button.dataset.room === room) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  }
  const self = room === 'self';
  $('chat-title').textContent = self ? '自己' : 'ACECode';
  $('chat-subtitle').textContent = self ? '自己的消息，也可以有回音。' : '通过 Channel 与独立测试会话对话';
  $('message-input').placeholder = self ? '给自己发条消息…' : '发送消息或 /session、/aq 等控制指令…';
  for (const id of ['commands', 'connection', 'channel-info']) $(id).hidden = self;
  $('empty').querySelector('h3').textContent = self ? '和自己说句话' : '从一条消息开始';
  $('empty').querySelector('p').textContent = self ? '发一条消息，服务端会原样回送。\n切换到 ACECode，开始测试 Channel。' : '消息将通过真实 Channel 发送给 ACECode。\n也可以先查看会话列表，测试绑定切换。';
  $('first-message').textContent = self ? '发送一条测试消息' : '查看会话列表';
  error();
  render(true);
  sync().catch(() => error('服务连接中断，请确认 IM 服务仍在运行。'));
  $('message-input').focus();
}

async function send(text = $('message-input').value) {
  if (sending || stopped || !text.trim()) return;
  if (new TextEncoder().encode(text).length > 16384) return error('消息过长，请限制在 16 KiB 以内。');
  if (room === 'acecode' && !connected && text.trim() !== '/help') return error('Channel 尚未连接，请先点击“连接 ACECode”。');
  const sentRoom = room;
  sending = true;
  $('send').disabled = true;
  $('send').firstChild.textContent = '发送中';
  error();
  try {
    const result = await api('/api/messages', { room: sentRoom, text, client_id: crypto.randomUUID() });
    if (result.ok) {
      if (room === sentRoom && $('message-input').value === text) $('message-input').value = '';
      if (drafts[sentRoom] === text) drafts[sentRoom] = '';
    } else error(result.error || '消息未能确认送达，请检查聊天记录。');
    await sync();
    if (room === sentRoom) scrollBottom();
  } catch (exc) { error(exc.name === 'TimeoutError' ? '请求超时，送达状态未知。请先检查记录，避免重复发送。' : exc.message); }
  finally {
    sending = false;
    $('send').disabled = false;
    $('send').firstChild.textContent = '发送';
  }
}

document.querySelectorAll('[data-room]').forEach((button) => button.addEventListener('click', () => selectRoom(button.dataset.room)));
document.querySelectorAll('[data-command]').forEach((button) => button.addEventListener('click', () => send(button.dataset.command)));
$('first-message').addEventListener('click', () => send(room === 'self' ? '你好，这是我的第一条自聊消息。' : '/session'));
$('send-form').addEventListener('submit', (event) => { event.preventDefault(); send(); });
$('message-input').addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing && event.keyCode !== 229) {
    event.preventDefault(); send();
  }
});
$('messages').addEventListener('scroll', () => {
  if ($('messages').scrollHeight - $('messages').scrollTop - $('messages').clientHeight < 80) $('jump').hidden = true;
});
$('jump').addEventListener('click', scrollBottom);
$('connection').addEventListener('click', async () => {
  const button = $('connection');
  button.disabled = true;
  error();
  try { await api('/api/connection', { connect: !connected }); await sync(); }
  catch (exc) { error(exc.message); }
  finally { button.disabled = false; }
});
$('older').addEventListener('click', async () => {
  const requestedRoom = room;
  const before = Math.min(...histories[room].keys());
  const height = $('messages').scrollHeight;
  try {
    const result = await api(`/api/history?room=${requestedRoom}&before=${before}`);
    for (const message of result.messages) histories[requestedRoom].set(message.id, message);
    if (room === requestedRoom) { render(); $('messages').scrollTop += $('messages').scrollHeight - height; }
    if (result.messages.length < 200) $('older').hidden = true;
  } catch (exc) { error(exc.message); }
});
$('export').addEventListener('click', async () => {
  try {
    const result = await api(`/api/export?room=${room}`);
    const url = URL.createObjectURL(new Blob([JSON.stringify(result, null, 2)], { type: 'application/json' }));
    const link = document.createElement('a');
    link.href = url; link.download = `im-${room}-${new Date().toISOString().slice(0, 10)}.json`; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } catch (exc) { error(exc.message); }
});
$('shutdown').addEventListener('click', async () => {
  if (!confirm('停止本次 IM 和它启动的测试 ACECode？聊天记录会保留。')) return;
  try {
    await api('/api/shutdown', {}); stopped = true; clearTimeout(pollTimer);
    $('service-status').textContent = '服务已停止'; $('service-dot').classList.remove('online');
    $('send').disabled = true; $('connection').disabled = true;
    error('测试服务已停止。重新运行启动命令即可继续使用，聊天记录已保存。');
  } catch (exc) { error(exc.message); }
});
poll();
