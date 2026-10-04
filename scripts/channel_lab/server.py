#!/usr/bin/env python3
"""Local, text-only IM and Channel v1 test adapter (Python standard library)."""
from __future__ import annotations

import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, build_opener, ProxyHandler
import webbrowser

ROOT = Path(__file__).resolve().parents[2]
ASSETS = Path(__file__).with_name('web')
MAX_TEXT = 16 * 1024
HTTP = build_opener(ProxyHandler({}))
HELP_TEXT = """可用指令

/help  显示这份指令帮助

会话导航（在 ACECode 会话使用）
/session  查看最近会话
/session all  查看全部会话（也可用 /session more）
/session 2  切换到上次列表中的第 2 个会话
/session search 关键词  搜索会话
/sessions、/resume 是 /session 的别名，用法相同。

回答问题（在 ACECode 会话使用）
/aq --status  查看待回答问题和回答进度
/aq 1  选择第 1 个选项，也可输入选项标签
/aq 1,3  多选第 1、3 个选项
/aq 我的回答  提交自定义答案
/aq --repeat  重新显示当前问题
/aq --back  返回上一题
/aq --cancel  取消当前这批提问

使用提示
“自己”会话原样回送普通文字；除 /help 外，不执行以上指令。
Channel 指令需要先连接 ACECode；/help 断开时也可查看。
右上角可连接或断开 ACECode、导出记录。
Enter 发送，Shift + Enter 换行。"""


class ApiError(Exception):
    def __init__(self, message, status=400):
        super().__init__(message)
        self.status = status


def request_json(url, data=None, headers=None, method=None, timeout=12):
    body = None if data is None else json.dumps(data, ensure_ascii=False).encode('utf-8')
    req = Request(url, data=body, method=method,
                  headers={'Content-Type': 'application/json', **(headers or {})})
    with HTTP.open(req, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8'))


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def require_text(value, name='text', limit=MAX_TEXT):
    if not isinstance(value, str) or not value.strip():
        raise ApiError(f'{name} 不能为空')
    if len(value.encode('utf-8')) > limit:
        raise ApiError(f'{name} 超过 {limit} 字节限制')
    return value


class Lab:
    def __init__(self, runtime):
        self.runtime = runtime
        runtime.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.action_lock = threading.Lock()
        self.db = sqlite3.connect(runtime / 'history.sqlite3', check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute('''CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT, room TEXT NOT NULL,
            sender TEXT NOT NULL, text TEXT NOT NULL, created REAL NOT NULL,
            status TEXT NOT NULL, kind TEXT NOT NULL DEFAULT 'text',
            session_id TEXT NOT NULL DEFAULT '', receipt TEXT UNIQUE)''')
        self.db.execute("UPDATE messages SET status='unknown' WHERE status='sending'")
        self.db.commit()
        self.binding = None
        self.admin_token = secrets.token_urlsafe(32)
        self.url = ''
        self.daemon_url = ''
        self.daemon_process = None
        self.daemon_env = None
        self.session_id = ''
        self.session_titles = {}
        self.startup_error = ''
        self.server = None

    def add(self, room, sender, text, status='received', kind='text', session_id='', receipt=None):
        with self.lock, self.db:
            cursor = self.db.execute('''INSERT OR IGNORE INTO messages
                (room,sender,text,created,status,kind,session_id,receipt) VALUES (?,?,?,?,?,?,?,?)''',
                (room, sender, text, time.time(), status, kind, session_id, receipt))
            return cursor.lastrowid

    def history(self, room, before=0, limit=200):
        if room not in ('self', 'acecode'):
            raise ApiError('未知会话')
        with self.lock:
            rows = self.db.execute('''SELECT id,room,sender,text,created,status,kind,session_id
                FROM messages WHERE room=? AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?''',
                (room, before, before, limit)).fetchall()
            return [dict(row) for row in reversed(rows)]

    def state(self, room):
        with self.lock:
            alive = self.daemon_process is None or self.daemon_process.poll() is None
            connected = self.binding is not None and alive
            sid = self.binding['session_id'] if connected else self.session_id
            return {'connected': connected, 'daemon_ready': bool(self.daemon_url) and alive,
                    'session_id': sid, 'session_title': self.session_titles.get(sid, ''),
                    'error': self.startup_error if alive else '测试 ACECode 已退出，请重启 IM 服务',
                    'messages': self.history(room), 'room': room}

    def lifecycle(self, data):
        if data.get('protocol_version') != 1:
            raise ApiError('unsupported protocol version')
        sid = require_text(data.get('session_id'), 'session_id', 256)
        with self.lock:
            if data.get('type') == 'channel.deactivate':
                if (self.binding and sid == self.binding['session_id'] and
                        data.get('binding_token') == self.binding['binding_token']):
                    self.binding = None
                return {'type': 'channel.status', 'state': 'connected'}
            if data.get('type') != 'channel.activate':
                raise ApiError('unsupported lifecycle request')
            inbound = data.get('inbound')
            if not isinstance(inbound, dict):
                raise ApiError('inbound required')
            parsed = urlsplit(require_text(inbound.get('url'), 'inbound.url', 1024))
            if (parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or
                    parsed.path != '/rc/send' or parsed.username or parsed.query or parsed.fragment):
                raise ApiError('inbound must be a loopback /rc/send URL')
            token = require_text(inbound.get('token'), 'token', 1024)
            if inbound.get('token_header', 'X-ACECode-RC-Token') != 'X-ACECode-RC-Token':
                raise ApiError('unsupported token header')
            same = bool(self.binding and self.binding['session_id'] == sid and
                        self.binding['url'] == inbound['url'] and self.binding['token'] == token)
            if not same:
                self.binding = {'session_id': sid, 'url': inbound['url'], 'token': token,
                                'receipt_namespace': secrets.token_hex(12),
                                'binding_token': secrets.token_urlsafe(32)}
            self.session_id = sid
            binding_token = self.binding['binding_token']
            return {'type': 'channel.status', 'state': 'connected', 'already_running': same,
                    'binding_token': binding_token,
                    'outbound': {'mode': 'webhook', 'url': self.url + '/webhook/' + binding_token}}

    def webhook(self, token, data):
        with self.lock:
            binding = self.binding
            if not binding or not hmac.compare_digest(token, binding['binding_token']):
                raise ApiError('inactive webhook', 403)
            if data.get('session_id') != binding['session_id']:
                raise ApiError('wrong session', 409)
            kind = require_text(data.get('type'), 'type', 100)
            text = data.get('text')
            if kind == 'tool_call':
                text = str(data.get('tool_name', 'tool')) + '\n' + str(data.get('args_preview', ''))
            text = require_text(text, limit=256 * 1024)
            seq = data.get('seq')
            receipt = f"out:{binding['receipt_namespace']}:{seq}" if isinstance(seq, int) else None
            self.add('acecode', 'assistant', text, kind=kind, session_id=binding['session_id'],
                     receipt=receipt)
        return {'ok': True}

    def send(self, data):
        room = data.get('room')
        if room not in ('self', 'acecode'):
            raise ApiError('未知会话')
        text = require_text(data.get('text'))
        client_id = require_text(data.get('client_id'), 'client_id', 128)
        receipt = f'in:{room}:{client_id}'
        # Serialize user submissions, but never block lifecycle/webhook callbacks on IO.
        with self.action_lock:
            with self.lock:
                previous = self.db.execute('SELECT id,status,text FROM messages WHERE receipt=?',
                                           (receipt,)).fetchone()
                if previous:
                    if previous['text'] != text:
                        raise ApiError('消息 ID 已用于另一条消息', 409)
                    return {'ok': previous['status'] == 'sent', 'id': previous['id'],
                            'status': previous['status']}
                if text.strip() == '/help':
                    # Local help remains available without a daemon or channel binding.
                    # Keep the request and reply atomic so a retry cannot lose the help.
                    with self.db:
                        self.db.executemany('''INSERT INTO messages
                            (room,sender,text,created,status,kind,receipt) VALUES (?,?,?,?,?,?,?)''',
                            [(room, 'me', text, time.time(), 'sent', 'text', receipt),
                             (room, 'system', HELP_TEXT, time.time(), 'received', 'help',
                              f'help:{room}:{client_id}')])
                        msg_id = self.db.execute('SELECT id FROM messages WHERE receipt=?',
                                                 (receipt,)).fetchone()['id']
                    return {'ok': True, 'id': msg_id, 'status': 'sent', 'error': ''}
                binding = dict(self.binding) if self.binding else None
                if room == 'acecode' and not self.state(room)['connected']:
                    raise ApiError('Channel 尚未连接，请先连接 ACECode', 409)
                sid = binding['session_id'] if room == 'acecode' else ''
                msg_id = self.add(room, 'me', text, status='sending', session_id=sid, receipt=receipt)
            status = 'sent'
            error = ''
            if room == 'self':
                self.add('self', 'echo', text, receipt=f'echo:{client_id}')
            else:
                try:
                    request_json(binding['url'], {'text': text, 'channel_message_id': client_id},
                                 {'X-ACECode-RC-Token': binding['token']}, timeout=10)
                except HTTPError as exc:
                    status, error = 'failed', f'ACECode 拒绝消息（HTTP {exc.code}）'
                except (OSError, URLError, ValueError):
                    status, error = 'unknown', '未确认送达，请检查回复后再决定是否重发'
            with self.lock, self.db:
                self.db.execute('UPDATE messages SET status=? WHERE id=?', (status, msg_id))
            return {'ok': status == 'sent', 'id': msg_id, 'status': status, 'error': error}

    def connection(self, connect):
        with self.action_lock:
            if not self.daemon_url or not self.session_id:
                raise ApiError('未启动测试 ACECode；请使用 --acecode 指定程序', 409)
            try:
                request_json(self.daemon_url + f'/api/sessions/{self.session_id}/commands',
                             {'command': 'rc' if connect else 'rc off'}, timeout=25)
            except (OSError, URLError, ValueError):
                raise ApiError('ACECode 连接操作失败，请查看运行目录中的 daemon.log', 502)
            return {'ok': True}


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(30)

    def log_message(self, *_):
        pass  # Request URLs may carry the webhook credential.

    @property
    def lab(self):
        return self.server.lab

    def respond(self, value, status=200, mime='application/json; charset=utf-8'):
        body = value if isinstance(value, bytes) else json.dumps(value, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        self.wfile.write(body)

    def check_origin(self):
        if self.headers.get('Host') != urlsplit(self.lab.url).netloc:
            raise ApiError('invalid host', 403)
        origin = self.headers.get('Origin')
        if origin is not None and origin != self.lab.url:
            raise ApiError('cross-origin request denied', 403)
        if self.headers.get('Sec-Fetch-Site') == 'cross-site':
            raise ApiError('cross-site request denied', 403)

    def do_GET(self):
        self.dispatch(False)

    def do_POST(self):
        self.dispatch(True)

    def dispatch(self, post):
        try:
            self.check_origin()
            parsed = urlsplit(self.path)
            route = parsed.path
            query = parse_qs(parsed.query)
            if not post:
                files = {'/': ('index.html', 'text/html; charset=utf-8'),
                         '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                         '/style.css': ('style.css', 'text/css; charset=utf-8')}
                if route in files:
                    name, mime = files[route]
                    return self.respond((ASSETS / name).read_bytes(), mime=mime)
                if route == '/api/health':
                    return self.respond({'ok': True, 'service': 'acecode-channel-lab'})
                room = query.get('room', ['self'])[0]
                if route == '/api/state':
                    return self.respond(self.lab.state(room))
                if route == '/api/history':
                    return self.respond({'messages': self.lab.history(room, int(query.get('before', ['0'])[0]))})
                if route == '/api/export':
                    return self.respond({'room': room, 'messages': self.lab.history(room, limit=-1)})
                raise ApiError('not found', 404)
            if self.headers.get_content_type() != 'application/json':
                raise ApiError('application/json required', 415)
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 512 * 1024:
                raise ApiError('invalid body size', 413)
            data = json.loads(self.rfile.read(size).decode('utf-8'))
            if not isinstance(data, dict):
                raise ApiError('JSON object required')
            if route == '/admin/lifecycle' or route == '/admin/shutdown':
                if (self.headers.get('Origin') is not None or not hmac.compare_digest(
                        self.headers.get('X-Lab-Token', ''), self.lab.admin_token)):
                    raise ApiError('invalid admin token', 403)
                if route == '/admin/lifecycle':
                    return self.respond(self.lab.lifecycle(data))
            elif route.startswith('/webhook/'):
                return self.respond(self.lab.webhook(route.removeprefix('/webhook/'), data))
            elif route == '/api/messages':
                return self.respond(self.lab.send(data))
            elif route == '/api/connection':
                if type(data.get('connect')) is not bool:
                    raise ApiError('connect must be boolean')
                return self.respond(self.lab.connection(data['connect']))
            elif route != '/api/shutdown':
                raise ApiError('not found', 404)
            self.respond({'ok': True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
        except ApiError as exc:
            self.respond({'error': str(exc)}, exc.status)
        except (ValueError, UnicodeError):
            self.respond({'error': 'invalid JSON or parameter'}, 400)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            self.respond({'error': '服务暂不可用，请检查运行目录及磁盘空间'}, 500)


def make_server(lab, port=0):
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = False
    server.lab = lab
    lab.server = server
    lab.url = f'http://127.0.0.1:{server.server_port}'
    return server


def source_config():
    data_dir = Path(os.environ['USERPROFILE']) / '.acecode'
    redirect = data_dir / 'data-dir.redirect.json'
    if redirect.exists():
        data_dir = Path(json.loads(redirect.read_text(encoding='utf-8-sig'))['data_dir'])
    return data_dir / 'config.json'


def start_daemon(lab, executable, model_name=None):
    if os.name != 'nt':
        raise ApiError('隔离 ACECode 启动目前支持 Windows；其他平台可单独运行 IM 和插件')
    profile = lab.runtime / 'profile'
    data_dir = profile / '.acecode'
    data_dir.mkdir(parents=True, exist_ok=True)
    workspace = lab.runtime / 'workspace'
    workspace.mkdir(exist_ok=True)
    source = source_config()
    config = json.loads(source.read_text(encoding='utf-8-sig')) if source.exists() else {}
    config = {k: v for k, v in config.items() if k in (
        'provider', 'openai', 'copilot', 'codex', 'saved_models', 'default_model_name',
        'context_window', 'toolchains')}
    if model_name:
        if model_name not in {item.get('name') for item in config.get('saved_models', [])}:
            raise ApiError('指定的模型配置不存在')
        config['default_model_name'] = model_name
    config.update({'mcp_servers': {}, 'skills': {'allowed': []},
                   'default_permission_mode': 'default', 'web': {'remote_enabled': False}})
    descriptor = lab.runtime / 'plugin-runtime.json'
    write_json(descriptor, {'url': lab.url, 'token': lab.admin_token})
    manifest = lab.runtime / 'channel-plugin.json'
    write_json(manifest, {'name': 'local-im-lab', 'schema': 'acecode.channel-plugin.v1',
                         'transport': 'stdio', 'command': sys.executable,
                         'args': [str(Path(__file__).with_name('plugin.py')), str(descriptor)],
                         'timeout_ms': 10000})
    config['remote_control'] = {'port': free_port(), 'default_channel': 'local-im-lab',
                               'channels': {'local-im-lab': {'manifest_path': str(manifest)}}}
    write_json(data_dir / 'config.json', config)
    env = os.environ.copy()
    env['USERPROFILE'] = str(profile)
    lab.daemon_env = env
    daemon_port = free_port()
    daemon_url = f'http://127.0.0.1:{daemon_port}'
    with (lab.runtime / 'daemon.log').open('ab') as log:
        lab.daemon_process = subprocess.Popen(
            [str(executable), 'daemon', '--foreground', f'--port={daemon_port}',
             f'--cwd={workspace}', f'--run-dir={lab.runtime / "daemon-run"}'],
            env=env, cwd=workspace, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW)
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if lab.daemon_process.poll() is not None:
            raise ApiError('测试 daemon 启动失败，详见运行目录 daemon.log')
        try:
            request_json(daemon_url + '/api/health', timeout=1)
            break
        except (OSError, ValueError):
            time.sleep(0.2)
    else:
        raise ApiError('等待测试 daemon 超时，详见运行目录 daemon.log')
    # Two ordinary test sessions let /session selection exercise a real rebind.
    session_file = lab.runtime / 'test-sessions.json'
    if session_file.exists():
        ids = json.loads(session_file.read_text(encoding='utf-8'))
        for sid in ids:
            request_json(daemon_url + f'/api/sessions/{sid}/resume', {}, timeout=25)
    else:
        ids = []
        for title in ('IM Channel 测试', 'IM 会话切换测试'):
            created = request_json(daemon_url + '/api/sessions', {}, timeout=25)
            sid = created['session_id']
            request_json(daemon_url + f'/api/sessions/{sid}/title', {'title': title}, method='PUT')
            ids.append(sid)
        write_json(session_file, ids)
    lab.daemon_url = daemon_url
    lab.session_id = ids[0]
    lab.session_titles = dict(zip(ids, ('IM Channel 测试', 'IM 会话切换测试')))
    lab.connection(True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--runtime-dir', type=Path,
                        default=Path(tempfile.gettempdir()) / 'acecode-channel-lab')
    parser.add_argument('--acecode', type=Path, help='Current-checkout ACECode executable')
    parser.add_argument('--model-name', help='Saved model name for new test sessions only')
    parser.add_argument('--open', action='store_true', help='Open the IM in a browser')
    parser.add_argument('--stop', action='store_true', help='Stop this runtime and its owned daemon')
    args = parser.parse_args()
    runtime = args.runtime_dir.resolve()
    owner = runtime / 'server.json'
    if args.stop:
        descriptor = json.loads(owner.read_text(encoding='utf-8'))
        request_json(descriptor['url'] + '/admin/shutdown', {}, {'X-Lab-Token': descriptor['token']})
        print('IM shutdown requested.')
        return
    runtime.mkdir(parents=True, exist_ok=True)
    # Bind a persistent runtime-specific lock so two instances cannot share the DB/config.
    lockfile = (runtime / 'server.lock').open('a+b')
    try:
        if os.name == 'nt':
            import msvcrt
            lockfile.seek(0)
            if lockfile.read(1) == b'':
                lockfile.write(b'0')
                lockfile.flush()
            lockfile.seek(0)
            msvcrt.locking(lockfile.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lockfile, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lockfile.close()
        try:
            descriptor = json.loads(owner.read_text(encoding='utf-8'))
            if not isinstance(descriptor, dict) or not isinstance(descriptor.get('url'), str):
                raise ValueError('invalid runtime address')
            address = urlsplit(descriptor['url'])
            if (address.scheme == 'http' and address.hostname == '127.0.0.1'
                    and address.port and not address.username and not address.password
                    and address.path in ('', '/') and not address.query and not address.fragment):
                print('IM 服务已在运行。')
                print(f'访问地址: http://127.0.0.1:{address.port}/')
                print('直接在浏览器打开上面的地址即可，无需重复启动。')
                return
        except (OSError, ValueError, KeyError, TypeError):
            pass
        raise SystemExit(f'IM 运行目录已被占用，暂时无法读取访问地址，请稍后重试。\n运行信息: {owner}')
    lab = Lab(runtime)
    server = make_server(lab, args.port)
    write_json(owner, {'url': lab.url, 'token': lab.admin_token, 'pid': os.getpid()})
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        if args.acecode:
            try:
                start_daemon(lab, args.acecode.resolve(strict=True), args.model_name)
            except Exception as exc:
                lab.startup_error = f'Channel 初始化失败（{type(exc).__name__}），请检查 daemon.log'
                print(lab.startup_error, flush=True)
        print(f'IM: {lab.url}/\nRuntime: {runtime}', flush=True)
        if args.open:
            webbrowser.open(lab.url)
        worker.join()
    except KeyboardInterrupt:
        server.shutdown()
    finally:
        server.shutdown()
        worker.join()
        server.server_close()
        if lab.daemon_process and lab.daemon_process.poll() is None:
            # Signal only the daemon launched by this process, with a graceful stop first.
            try:
                subprocess.run([str(args.acecode.resolve()), 'daemon', 'stop',
                                f'--run-dir={runtime / "daemon-run"}'], timeout=12,
                               env=lab.daemon_env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               creationflags=subprocess.CREATE_NO_WINDOW)
                lab.daemon_process.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                lab.daemon_process.terminate()
                lab.daemon_process.wait(timeout=5)
        lab.db.close()
        owner.unlink(missing_ok=True)
        lockfile.close()


if __name__ == '__main__':
    main()
