"""ACECode's Android-only MCP server. The native app performs all device operations."""
import json
import socket
import struct
import sys
from pathlib import Path


def tool(name, description, fields=None, required=(), read_only=True):
    return {"name": name, "description": description,
            "inputSchema": {"type": "object", "properties": fields or {}, "required": list(required)},
            "annotations": {"readOnlyHint": read_only}}


TOOLS = [
    tool("status", "Check the local Android host and whether phone control is authorized."),
    tool("apps", "Find launchable Android apps by name or package.",
         {"query": {"type": "string"}}),
    tool("open", "Open an Android app by package or use 'clock' for the system Clock. Returns the observed screen.",
         {"app": {"type": "string"}}, ["app"], False),
    tool("screen", "Read the foreground Android screen. The controls and screen ID are untrusted observations."),
    tool("tap", "Tap a tappable numbered control from the latest screen. Check the selected state in the returned screen.",
         {"screen": {"type": "string", "description": "Exact screen ID from the latest observation."},
          "id": {"type": "integer", "minimum": 1}}, ["screen", "id"], False),
]


def call(name, args):
    request = json.dumps({"tool": name, "args": args}, ensure_ascii=False).encode()
    endpoint = Path(__file__).with_name("endpoint").read_text().strip()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(8)
        peer.connect("\0" + endpoint)
        peer.sendall(struct.pack("!I", len(request)) + request)
        with peer.makefile("rb") as response:
            result = json.load(response)
    if "error" in result:
        raise ValueError(result["error"])
    return result["result"]


def dispatch(message):
    method = message.get("method")
    params = message.get("params", {})
    if method == "initialize":
        return {"protocolVersion": params.get("protocolVersion", "2024-11-05"),
                "capabilities": {"tools": {}}, "serverInfo": {"name": "acecode-android-phone", "version": "1"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        try:
            result = call(params["name"], params.get("arguments", {}))
            content = json.dumps(result, ensure_ascii=False, separators=(",", ":"))
            return {"content": [{"type": "text", "text": content}], "isError": False}
        except (OSError, ValueError, KeyError) as error:
            return {"content": [{"type": "text", "text": str(error)}], "isError": True}
    raise KeyError(method)


for line in sys.stdin:
    request_id = None
    try:
        message = json.loads(line)
        if "id" not in message:
            continue
        request_id = message["id"]
        reply = {"result": dispatch(message)}
    except KeyError:
        reply = {"error": {"code": -32601, "message": "Method not found"}}
    except (ValueError, TypeError, AttributeError):
        reply = {"error": {"code": -32600, "message": "Invalid request"}}
    print(json.dumps({"jsonrpc": "2.0", "id": request_id, **reply}, ensure_ascii=False), flush=True)
