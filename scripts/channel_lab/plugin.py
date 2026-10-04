#!/usr/bin/env python3
"""Short-lived Channel v1 stdio process. Never print credentials or diagnostics."""
import json
from pathlib import Path
import sys

from server import request_json


def main():
    try:
        descriptor = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
        request = json.loads(sys.stdin.buffer.readline(64 * 1024).decode('utf-8'))
        result = request_json(descriptor['url'] + '/admin/lifecycle', request,
                              {'X-Lab-Token': descriptor['token']}, timeout=8)
    except Exception:
        result = {'type': 'channel.status', 'state': 'failed',
                  'message': 'Local IM is unavailable; start the channel lab server.'}
    sys.stdout.buffer.write((json.dumps(result, ensure_ascii=False) + '\n').encode('utf-8'))
    sys.stdout.buffer.flush()
    return 0 if result.get('state') == 'connected' else 1


if __name__ == '__main__':
    raise SystemExit(main())
