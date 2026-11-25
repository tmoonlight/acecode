#!/usr/bin/env python3
"""自动模式（试验）的真实 daemon 验收(权限模式 auto-review)。

用法:
    python -I tests/scripts/auto_review_acceptance_test.py --binary <acecode 可执行文件>

隔离环境:临时用户目录、临时工作区、临时 run 目录;主模型与裁决模型都由本文件里的假
OpenAI 兼容服务扮演,不访问外网。核对的路径:
  1. 裁决放行后命令执行,裁决请求发给配置的裁决模型、不带工具、非流式;
  2. 裁决拦下后对话里出现带理由的失败结果,没有权限请求,工具输出没有进裁决请求;
  3. 裁决服务返回 429(没有结果)时退回权限请求,请求里带原因;
  4. 切出模式后给主模型的说明被收回,不再发裁决请求;
  5. 改成「跟随当前模型」后裁决发给会话模型,config.json 不再有 auto_review 段;
  6. 裁决进行中打断回合:回合立刻停下,命令没有执行;
  7. 设置接口的读写与 400,审计里有来源为 classifier 的记录;
  8. headless(无人可问):裁决放行照常执行,裁决没有结果时拒绝并把说明回给模型。

注意:免确认的安全命令(例如 echo)在这个模式里直接放行,不会送去裁决;脚本里用
`python -c` 当「需要确认的命令」,并把沙盒关掉让它落进「要确认」那一档。

`--serve-fake <端口>` 只启动假模型服务,供 web/scripts/test-auto-review-mode.mjs 复用。
"""
import argparse
import base64
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESULTS = []

# ---------------------------------------------------------------------------
# 假模型服务:同时扮演主模型和裁决模型
#   主模型:看最后一条含 `RUN: <命令>` 的 user 消息,第一次请求回一个 bash 工具调用,
#          拿到工具结果后回一句文本结束回合;不带工具表的请求是标题之类的旁路请求。
#   裁决模型:系统提示以 "You are the safety reviewer" 开头的请求。待裁决的命令
#          含 `push --force` → 两个阶段都判拦;含 `REVIEW429` → 返回 HTTP 429;
#          含 `SLOWREVIEW` → 等 5 秒再放行;其余放行。
#   GET /__log 返回收到的请求摘要,GET /__reset 清空。
# ---------------------------------------------------------------------------
LOG = []
LOCK = threading.Lock()


def text_of(message):
    content = message.get("content")
    if isinstance(content, list):
        return "".join(part.get("text", "") for part in content if isinstance(part, dict))
    return content or ""


def last_run_text(messages):
    for message in reversed(messages):
        if message.get("role") == "user" and "RUN:" in text_of(message):
            return text_of(message)
    return ""


class FakeModel(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send_json(self, status, body):
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send_sse(self, chunks):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()
        for chunk in chunks:
            self.wfile.write(("data: " + json.dumps(chunk) + "\n\n").encode("utf-8"))
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True

    def _reply_text(self, text, stream, model):
        usage = {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
        if not stream:
            return self._send_json(200, {"id": "c", "object": "chat.completion", "model": model, "choices": [{
                "index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": text}}],
                "usage": usage})
        return self._send_sse([
            {"id": "c", "object": "chat.completion.chunk", "model": model,
             "choices": [{"index": 0, "delta": {"role": "assistant", "content": text}, "finish_reason": None}]},
            {"id": "c", "object": "chat.completion.chunk", "model": model,
             "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": usage},
        ])

    def do_GET(self):
        if self.path == "/__log":
            with LOCK:
                return self._send_json(200, LOG)
        if self.path == "/__reset":
            with LOCK:
                LOG.clear()
            return self._send_json(200, {"ok": True})
        return self._send_json(200, {"data": [{"id": "main-model"}, {"id": "reviewer-model"}]})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        system = text_of(messages[0]) if messages and messages[0].get("role") == "system" else ""
        stream = bool(body.get("stream"))
        model = body.get("model", "")
        offered = len(body.get("tools") or [])

        if system.startswith("You are the safety reviewer"):
            user = text_of(messages[-1])
            action = user.split("<action>", 1)[-1]
            stage = "quick" if "First pass." in user else "full"
            with LOCK:
                LOG.append({"kind": "review", "model": model, "stage": stage, "stream": stream,
                            "tools": offered, "user": user})
            if "SLOWREVIEW" in action:
                time.sleep(5)
            if "REVIEW429" in action:
                return self._send_json(429, {"error": {"message": "rate limited", "type": "rate_limit"}})
            if "push --force" in action:
                text = "<block>yes</block>" if stage == "quick" else \
                    "<block>yes</block><reason>B2: force-push rewrites published history</reason>"
            else:
                text = "<block>no</block>"
            return self._reply_text(text, stream, model)

        last = messages[-1] if messages else {}
        context = [text_of(m) for m in messages if m.get("role") == "user"]
        with LOCK:
            # 带工具表的才是会话回合;不带工具的是标题、预测消息之类的旁路请求。
            LOG.append({"kind": "main" if offered else "side", "model": model, "stream": stream,
                        "has_guidance": any("Auto mode (experimental) is on." in t for t in context),
                        "withdrawn": any("<auto_review>\nThis section is no longer active." in t for t in context),
                        "system_has_guidance": "Auto mode (experimental) is on." in system,
                        "last_tool": text_of(last)[:600] if last.get("role") == "tool" else ""})
        if not offered:
            return self._reply_text("side reply", stream, model)
        if last.get("role") == "tool":
            return self._reply_text("done", stream, model)
        match = re.search(r"RUN: (.+)", last_run_text(messages))
        if not match or last.get("role") != "user":
            return self._reply_text("nothing to run", stream, model)
        call = {"id": "call_%d" % len(messages), "type": "function",
                "function": {"name": "bash", "arguments": json.dumps({"command": match.group(1).strip()})}}
        usage = {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
        if not stream:
            return self._send_json(200, {"id": "c", "object": "chat.completion", "model": model, "choices": [{
                "index": 0, "finish_reason": "tool_calls",
                "message": {"role": "assistant", "content": None, "tool_calls": [call]}}], "usage": usage})
        return self._send_sse([
            {"id": "c", "object": "chat.completion.chunk", "model": model, "choices": [{"index": 0, "delta": {
                "role": "assistant", "tool_calls": [{"index": 0, **call}]}, "finish_reason": None}]},
            {"id": "c", "object": "chat.completion.chunk", "model": model,
             "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}], "usage": usage},
        ])


# ---------------------------------------------------------------------------
# 最小 WebSocket 客户端(只支持文本帧):权限请求的应答只有 WebSocket 一条路
# ---------------------------------------------------------------------------
class MiniWs:
    def __init__(self, host, port, path, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(request.encode("ascii"))
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("websocket handshake closed")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise RuntimeError("websocket handshake failed: " + head[:200].decode("latin-1"))
        self.buffer = head.split(b"\r\n\r\n", 1)[1]

    def send_json(self, value):
        data = json.dumps(value).encode("utf-8")
        mask = os.urandom(4)
        header = bytes([0x81])
        if len(data) < 126:
            header += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", len(data))
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", len(data))
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("websocket closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:count], self.buffer[count:]
        return out

    def recv_json(self, timeout):
        """读下一条文本帧并解析成 JSON;超时返回 None。"""
        self.sock.settimeout(timeout)
        try:
            text = b""
            while True:
                first, second = self._read(2)
                opcode, final = first & 0x0F, bool(first & 0x80)
                length = second & 0x7F
                if length == 126:
                    length = struct.unpack(">H", self._read(2))[0]
                elif length == 127:
                    length = struct.unpack(">Q", self._read(8))[0]
                mask = self._read(4) if second & 0x80 else None
                payload = self._read(length)
                if mask:
                    payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                if opcode == 0x8:
                    raise RuntimeError("websocket closed by server")
                if opcode in (0x9, 0xA):
                    continue
                text += payload
                if final:
                    break
            return json.loads(text.decode("utf-8", "replace"))
        except socket.timeout:
            return None

    def wait_for(self, predicate, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame = self.recv_json(max(0.1, deadline - time.monotonic()))
            if frame is not None and predicate(frame):
                return frame
        return None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 验收
# ---------------------------------------------------------------------------
def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def http(method, url, body=None, token=None, timeout=20):
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    if token:
        request.add_header("X-ACECode-Token", token)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            return response.status, (json.loads(raw) if raw else None)
    except urllib.error.HTTPError as error:
        raw = error.read()
        try:
            return error.code, json.loads(raw)
        except Exception:
            return error.code, raw.decode("utf-8", "replace")


def check(name, condition, detail=""):
    RESULTS.append((name, bool(condition)))
    print(("PASS " if condition else "FAIL ") + name
          + ((" :: " + str(detail)[:300]) if detail and not condition else ""), flush=True)


def wait_idle(url, token, session, timeout=60):
    deadline = time.monotonic() + timeout
    snapshot = None
    while time.monotonic() < deadline:
        _, snapshot = http("GET", f"{url}/api/sessions/{session}/messages", token=token)
        if snapshot and not snapshot.get("busy"):
            return snapshot
        time.sleep(0.3)
    return snapshot


def tool_messages(snapshot):
    return [m for m in (snapshot or {}).get("messages", []) if m.get("role") == "tool"]


def send(url, token, session, text):
    status, body = http("POST", f"{url}/api/sessions/{session}/messages", {"text": text}, token=token)
    if status >= 300:
        raise RuntimeError(f"send failed {status} {body}")
    time.sleep(0.4)


def reviews_of(base):
    return [entry for entry in http("GET", base + "/__log")[1] if entry["kind"] == "review"]


def turns_of(base):
    return [entry for entry in http("GET", base + "/__log")[1] if entry["kind"] == "main"]


def run_checks(url, token, base, profile, workspace):
    # 设置接口
    status, settings = http("GET", url + "/api/config/auto-review", token=token)
    check("GET /api/config/auto-review returns the configured reviewer",
          status == 200 and settings.get("model_name") == "reviewer" and settings.get("configured") is True, settings)
    check("settings response carries no credentials", "fixture" not in json.dumps(settings), settings)
    status, body = http("PUT", url + "/api/config/auto-review", {"model_name": "no-such-model"}, token=token)
    check("PUT with an unknown model name returns 400", status == 400, (status, body))
    status, settings = http("GET", url + "/api/config/auto-review", token=token)
    check("a rejected PUT leaves the value unchanged", settings.get("model_name") == "reviewer", settings)

    status, created = http("POST", url + "/api/sessions", {"permission_mode": "auto-review", "model": "main"},
                           token=token)
    session = (created or {}).get("session_id") or ""
    check("session created in auto-review mode", status in (200, 201) and session, (status, created))
    status, mode = http("GET", f"{url}/api/sessions/{session}/permissions", token=token)
    check("GET permissions reports auto-review", status == 200 and mode.get("mode") == "auto-review", mode)

    # 1. 裁决放行。输出靠字符串拼接得到,命令原文里没有这串字,后面据此断言工具输出没进裁决请求。
    http("GET", base + "/__reset")
    allow_command = '''python -c "print('review-allowed-'+'marker')"'''
    send(url, token, session, "RUN: " + allow_command)
    tools = tool_messages(wait_idle(url, token, session))
    reviews = reviews_of(base)
    check("allow: the command ran", len(tools) == 1 and "review-allowed-marker" in tools[0].get("content", ""),
          tools[-1:])
    check("allow: exactly one review request (first stage)",
          len(reviews) == 1 and reviews[0]["stage"] == "quick", reviews)
    check("allow: the review went to the configured reviewer model",
          reviews and reviews[0]["model"] == "reviewer-model", reviews[:1])
    check("allow: the review request is tool-free and non-streaming",
          reviews and reviews[0]["tools"] == 0 and reviews[0]["stream"] is False, reviews[:1])
    seen = reviews[0]["user"] if reviews else ""
    check("allow: the reviewer saw the user's words and the pending command",
          "User: RUN: " + allow_command in seen and "<action>\nTool bash" in seen
          and "Execution: with full access (no sandbox)" in seen, seen[:600])
    turns = turns_of(base)
    check("guidance: the main model request carries the auto mode note outside the system prompt",
          turns and turns[0]["has_guidance"] and not turns[0]["system_has_guidance"], turns[:1])

    # 2. 裁决拦下
    http("GET", base + "/__reset")
    send(url, token, session, "RUN: git push --force origin main")
    tools = tool_messages(wait_idle(url, token, session))
    blocked = tools[-1] if tools else {}
    content = blocked.get("content", "")
    check("block: the command did not run and the result explains why",
          len(tools) == 2 and "[Auto mode]" in content and "B2: force-push rewrites published history" in content,
          content[:400])
    check("block: tool result metadata marks an auto review block",
          (blocked.get("metadata") or {}).get("auto_review", {}).get("blocked") is True, blocked.get("metadata"))
    reviews = reviews_of(base)
    check("block: two review requests (both stages)", [r["stage"] for r in reviews] == ["quick", "full"], reviews)
    check("block: earlier tool output never reached the reviewer",
          reviews and all("review-allowed-marker" not in r["user"] for r in reviews)
          and "print('review-allowed-'+'marker')" in reviews[0]["user"], reviews[0]["user"][:800] if reviews else "")

    # 3. 裁决没有结果 → 权限请求(经 WebSocket 收到并作答)
    http("GET", base + "/__reset")
    ws = MiniWs("127.0.0.1", int(url.rsplit(":", 1)[1]), f"/ws/sessions/_multiplex?token={token}")
    ws.send_json({"type": "subscribe", "payload": {"session_id": session, "since": 0}})
    progress = []
    send(url, token, session, '''RUN: python -c "print('REVIEW429')"''')

    def is_request(frame):
        if frame.get("type") == "agent_progress":
            progress.append((frame.get("payload") or {}).get("phase"))
        return frame.get("type") == "permission_request"

    request = ws.wait_for(is_request, 40)
    check("no verdict: a permission request reaches the client", request is not None, progress[-6:])
    payload = (request or {}).get("payload") or {}
    note = ((payload.get("args") or {}).get("permission") or {}).get("auto_review") or {}
    check("no verdict: the request explains that the automatic check gave no result",
          note.get("why") == "no_verdict" and note.get("reason") == "HTTP 429", payload)
    check("progress: a safety-check phase was announced before the prompt",
          "permission_reviewing" in progress, progress)
    ws.send_json({"type": "decision", "payload": {"session_id": session,
                                                  "request_id": payload.get("request_id", ""), "choice": "deny"}})
    tools = tool_messages(wait_idle(url, token, session))
    ws.close()
    check("no verdict: the denied command did not run",
          tools and "[User denied tool execution]" in tools[-1].get("content", ""), tools[-1:])

    # 4. 切出模式:说明被收回,不再裁决
    status, mode = http("PUT", f"{url}/api/sessions/{session}/permissions", {"mode": "yolo"}, token=token)
    check("mode switch to yolo accepted", status == 200 and mode.get("mode") == "yolo", mode)
    http("GET", base + "/__reset")
    send(url, token, session, "RUN: echo after-mode-switch")
    wait_idle(url, token, session)
    check("after leaving the mode: no review requests", len(reviews_of(base)) == 0)
    turns = turns_of(base)
    check("after leaving the mode: the note is withdrawn with a context update",
          turns and turns[0]["withdrawn"], turns[:1])

    # 5. 改成跟随当前模型
    status, settings = http("PUT", url + "/api/config/auto-review", {"model_name": ""}, token=token)
    check("PUT empty model name succeeds", status == 200 and settings.get("model_name") == "", (status, settings))
    saved = json.loads((profile / ".acecode" / "config.json").read_text(encoding="utf-8"))
    check("config.json omits the auto_review section when following the session model",
          "auto_review" not in saved, list(saved.keys()))
    http("PUT", f"{url}/api/sessions/{session}/permissions", {"mode": "auto-review"}, token=token)
    http("GET", base + "/__reset")
    send(url, token, session, '''RUN: python -c "print('follow-'+'session-model')"''')
    wait_idle(url, token, session)
    reviews = reviews_of(base)
    check("follow current model: the review went to the session model",
          reviews and reviews[0]["model"] == "main-model", reviews[:1])

    # 6. 裁决进行中打断回合。假模型对这条命令要等 5 秒才回答;打断后回合应立刻停下,
    #    命令不执行(用「有没有写出文件」判断)。
    http("GET", base + "/__reset")
    marker = workspace / "interrupted-ran.txt"
    ws = MiniWs("127.0.0.1", int(url.rsplit(":", 1)[1]), f"/ws/sessions/_multiplex?token={token}")
    ws.send_json({"type": "subscribe", "payload": {"session_id": session, "since": 0}})
    send(url, token, session, '''RUN: python -c "open('interrupted-ran.txt','w').write('SLOWREVIEW')"''')
    deadline = time.monotonic() + 20
    while not reviews_of(base) and time.monotonic() < deadline:
        time.sleep(0.1)
    check("interrupt: the review request is in flight", len(reviews_of(base)) == 1)
    started = time.monotonic()
    ws.send_json({"type": "abort", "payload": {"session_id": session}})
    snapshot = wait_idle(url, token, session, timeout=15)
    elapsed = time.monotonic() - started
    ws.close()
    check("interrupt: the turn stops without waiting for the reviewer",
          snapshot is not None and not snapshot.get("busy") and elapsed < 3.0, f"{elapsed:.1f}s")
    time.sleep(5.5)  # 等假模型那 5 秒过去,确认迟到的回答不会让命令执行
    check("interrupt: the command never ran", not marker.exists())

    # 7. 审计
    status, audit = http("GET", url + "/api/security/audit?limit=50", token=token)
    entries = (audit or {}).get("entries") or (audit or {}).get("items") or (audit if isinstance(audit, list) else [])
    sources = [(e.get("decision"), e.get("source"), e.get("reason")) for e in entries]
    check("audit records classifier decisions",
          ("allow", "classifier", "review_stage1") in sources and ("deny", "classifier", "review_stage2") in sources,
          sources[:12])
    check("audit records the interrupted review", ("deny", "none", "review_interrupted") in sources, sources[:12])


def run_headless_checks(staged, env, base, workspace):
    """8. headless:没有人能确认。此时配置是「跟随当前模型」,裁决发给会话模型。"""
    def headless(command):
        http("GET", base + "/__reset")
        return subprocess.run([str(staged), "-p", "RUN: " + command, "--permission-mode", "auto-review",
                               "--model", "main", "--output-format", "json"],
                              cwd=workspace, env=env, capture_output=True, timeout=120)

    done = headless('''python -c "open('headless-allowed.txt','w').write('ok')"''')
    check("headless allow: exits cleanly", done.returncode == 0, done.stdout[-400:] + done.stderr[-400:])
    check("headless allow: the reviewed command ran", (workspace / "headless-allowed.txt").exists())
    check("headless allow: one review request", len(reviews_of(base)) == 1, reviews_of(base))

    done = headless('''python -c "open('headless-429.txt','w').write('REVIEW429')"''')
    check("headless no verdict: exits instead of waiting for an answer", done.returncode == 0,
          done.stdout[-400:] + done.stderr[-400:])
    check("headless no verdict: the command did not run", not (workspace / "headless-429.txt").exists())
    told = [entry["last_tool"] for entry in turns_of(base) if entry["last_tool"]]
    check("headless no verdict: the model is told the review gave no result",
          told and "the safety review could not reach a decision" in told[-1], told[-1:] )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--serve-fake", type=int, metavar="PORT")
    args = parser.parse_args()
    if args.serve_fake:
        ThreadingHTTPServer(("127.0.0.1", args.serve_fake), FakeModel).serve_forever()
        return 0
    if not args.binary:
        parser.error("--binary is required")
    binary = args.binary.resolve()

    # 短的真实路径,避开目录联接与 MAX_PATH。
    temp_base = binary.anchor if os.name == "nt" else None
    with tempfile.TemporaryDirectory(prefix="ace-review-", dir=temp_base) as temporary:
        root = Path(temporary)
        package = root / "package"
        package.mkdir()
        staged = package / binary.name
        shutil.copy2(binary, staged)
        for resource in ["seed", "models_dev"]:
            shutil.copytree(ROOT / "assets" / resource, package / "share/acecode" / resource)
        profile = root / "profile"
        (profile / ".acecode").mkdir(parents=True)
        (profile / "AppData").mkdir()
        workspace = root / "workspace"
        workspace.mkdir()

        model_port = free_port()
        base = f"http://127.0.0.1:{model_port}"
        saved = [{"name": name, "provider": "openai", "base_url": base + "/v1", "api_key": "fixture",
                  "model": name + "-model"} for name in ["main", "reviewer"]]
        config = {"saved_models": saved, "default_model_name": "main",
                  "sandbox": {"enabled": False}, "migrations": {"disable_sandbox_once": True},
                  "network": {"proxy_mode": "off"}, "auto_review": {"model_name": "reviewer"}}
        (profile / ".acecode" / "config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")

        env = {**os.environ, "HOME": str(profile), "USERPROFILE": str(profile),
               "APPDATA": str(profile / "AppData"), "LOCALAPPDATA": str(profile / "AppData"),
               "NO_PROXY": "127.0.0.1,localhost", "HTTP_PROXY": "", "HTTPS_PROXY": "", "ALL_PROXY": ""}
        fake = subprocess.Popen([sys.executable, "-I", str(Path(__file__).resolve()), "--serve-fake", str(model_port)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = free_port()
        url = f"http://127.0.0.1:{port}"
        token = "review-accept-fixture"
        log_path = root / "daemon.log"
        daemon = None
        try:
            with log_path.open("wb") as log:
                daemon = subprocess.Popen([str(staged), "daemon", "--foreground", f"--port={port}",
                                           f"--token={token}", f"--cwd={workspace}", f"--run-dir={root / 'run'}"],
                                          cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 40
                while True:
                    try:
                        if http("GET", url + "/api/health", token=token, timeout=1)[0] == 200:
                            break
                    except Exception:
                        pass
                    if daemon.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError("daemon did not start")
                    time.sleep(0.3)
                run_checks(url, token, base, profile, workspace)
                run_headless_checks(staged, env, base, workspace)
        finally:
            if daemon and daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
            fake.terminate()
            if any(not ok for _, ok in RESULTS) and log_path.exists():
                print("---- daemon log tail ----\n" + log_path.read_text(encoding="utf-8", errors="replace")[-3000:])
    failed = [name for name, ok in RESULTS if not ok]
    print(f"\n{len(RESULTS) - len(failed)}/{len(RESULTS)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
