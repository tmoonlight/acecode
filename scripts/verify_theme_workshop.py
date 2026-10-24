#!/usr/bin/env python3
"""Exercise the real workshop through an isolated Windows ACECode daemon.

No production configuration or workshop data is changed. --serve keeps the
isolated daemon alive for UI verification; stop it using the printed PID.
"""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--update-url', default='http://2017studio.imwork.net:82/aupdate/')
    parser.add_argument('--serve', action='store_true')
    args = parser.parse_args()
    if os.name != 'nt':
        raise SystemExit('This isolated USERPROFILE harness is Windows-only.')
    root = Path(tempfile.mkdtemp(prefix='ace_workshop_verify_'))
    data = root / '.acecode'
    data.mkdir()
    (data / 'config.json').write_text(json.dumps({'upgrade': {'base_url': args.update_url, 'auto_check': False}}), encoding='utf-8')
    workspace = root / 'workspace'
    workspace.mkdir()
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    token = secrets.token_hex(24)
    base = f'http://127.0.0.1:{port}'
    env = dict(os.environ, USERPROFILE=str(root))
    static = Path(__file__).resolve().parents[1] / 'web' / 'dist'
    log = (root / 'daemon.log').open('wb')
    process = subprocess.Popen([str(args.binary.resolve()), 'daemon', '--foreground', f'--port={port}',
        f'--token={token}', f'--cwd={workspace}', f'--run-dir={root / "run"}', f'--static-dir={static}'],
        env=env, stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
    checks = []

    def request(path, method='GET', payload=None, binary=False):
        body = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(base + path, body, {'Authorization': f'Bearer {token}', 'Content-Type': 'application/json'}, method=method)
        with urllib.request.urlopen(req, timeout=45) as response:
            content = response.read()
            return content if binary else json.loads(content)

    def check(condition, name):
        if not condition:
            raise AssertionError(name)
        checks.append(name)

    keep = False
    try:
        for _ in range(90):
            if process.poll() is not None:
                raise RuntimeError(f'Daemon exited; see {root / "daemon.log"}')
            try:
                request('/api/config/upgrade')
                break
            except (OSError, ValueError):
                time.sleep(1)
        else:
            raise RuntimeError('Daemon did not become ready')
        catalog = request('/api/themes?refresh=1')
        offers = [entry for entry in catalog['themes'] if entry.get('source') == 'workshop']
        check(bool(offers), 'Public workshop offers appear in the client catalog')
        check(len({entry['id'] for entry in catalog['themes']}) == len(catalog['themes']), 'No duplicate theme IDs')
        theme = offers[0]
        check(not theme['installed'], 'Fresh profile has not installed the workshop theme')
        png = request(f'/api/themes/workshop/{theme["key"]}/thumbnail', binary=True)
        check(png.startswith(b'\x89PNG\r\n\x1a\n'), 'Authenticated thumbnail proxy returns PNG')
        request(f'/api/themes/{theme["id"]}/install', 'POST', {'confirm_download': True, 'version': theme['version'],
            'bytes': theme['package']['bytes'], 'sha256': theme['package']['sha256']})
        for _ in range(180):
            job = request('/api/themes/job')
            if job['state'] not in ('downloading', 'installing'):
                break
            time.sleep(0.25)
        check(job['state'] == 'completed', f'Verified installation completes: {job.get("error", "ok")}')
        definition = request(f'/api/themes/{theme["id"]}')
        check(definition['id'] == theme['id'] and definition['version'] == theme['version'], 'Installed identity and version match')
        check(request(f'/api/themes/{theme["id"]}/images/background', binary=True).startswith(b'\x89PNG'), 'Installed background is readable')
        refreshed = request('/api/themes?refresh=1')['themes']
        matching = [entry for entry in refreshed if entry['id'] == theme['id']]
        check(len(matching) == 1 and matching[0]['installed'], 'Refresh merges installed state into one workshop card')
        request(f'/api/themes/{theme["id"]}', 'DELETE')
        restored = next(entry for entry in request('/api/themes?refresh=1')['themes'] if entry['id'] == theme['id'])
        check(not restored['installed'], 'Deleting the isolated install leaves the public offer available')
        report = {'checks': checks, 'theme': {'id': theme['id'], 'name': theme['name'], 'version': theme['version']},
            'root': str(root), 'base_url': base, 'pid': process.pid}
        (root / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        (root / 'browser-auth.txt').write_text(token, encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False), flush=True)
        keep = args.serve
    finally:
        if not keep:
            process.terminate()
            process.wait(timeout=15)
        log.close()


if __name__ == '__main__':
    main()
