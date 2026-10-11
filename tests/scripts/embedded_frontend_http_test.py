#!/usr/bin/env python3
"""Probe a built daemon's embedded assets in an isolated staged profile."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--playwright-module')
    parser.add_argument('--build-version', help='embedded build hash (defaults to current checkout)')
    args = parser.parse_args()
    binary = args.binary.resolve()
    # A short, real path avoids junction and MAX_PATH issues during seed setup.
    temp_base = binary.anchor if os.name == 'nt' else None
    with tempfile.TemporaryDirectory(prefix='ace-http-', dir=temp_base) as temporary:
        root = Path(temporary)
        package = root / 'package'
        package.mkdir()
        staged = package / binary.name
        shutil.copy2(binary, staged)
        for resource in ['seed', 'models_dev']:
            shutil.copytree(ROOT / 'assets' / resource, package / 'share/acecode' / resource)
        profile = root / 'profile'
        profile.mkdir()
        workspace = root / 'workspace'
        workspace.mkdir()
        env = {**os.environ, **{key: str(profile) for key in ['HOME', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA']}}
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        url = f'http://127.0.0.1:{port}'
        with (root / 'daemon.log').open('wb') as log:
            child = subprocess.Popen([str(staged), 'daemon', '--foreground',
                                      f'--port={port}', '--token=embedded-asset-fixture',
                                      f'--cwd={workspace}', f'--run-dir={root / "run"}'],
                                     cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 35
                while True:
                    try:
                        with urllib.request.urlopen(url + '/api/health', timeout=1) as response:
                            if response.status == 200:
                                break
                    except OSError:
                        if child.poll() is not None or time.monotonic() > deadline:
                            raise AssertionError(f'isolated daemon failed to start (exit={child.poll()})')
                        time.sleep(.1)

                def fetch(encoding, path='/'):
                    request = urllib.request.Request(url + path, headers={'Accept-Encoding': encoding})
                    with urllib.request.urlopen(request, timeout=15) as response:
                        return response.headers, response.read()

                compressed_headers, compressed = fetch('gzip')
                assert compressed_headers.get('Content-Encoding') == 'gzip'
                assert compressed_headers.get('Vary') == 'Accept-Encoding'
                original = gzip.decompress(compressed)
                version = args.build_version or subprocess.check_output(
                    ['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], text=True).strip()
                expected = (ROOT / 'web/dist/index.html').read_bytes().replace(
                    b'__VERSION__', version.encode('utf-8'))
                assert original == expected, 'embedded page is stale or truncated'
                assert len(compressed) < len(original) // 2
                with ThreadPoolExecutor(max_workers=4) as pool:
                    for headers, data in pool.map(fetch, ['', 'identity', 'gzip;q=0', '*;q=1, gzip;q=0']):
                        assert headers.get('Content-Encoding') is None
                        assert headers.get('Vary') == 'Accept-Encoding'
                        assert headers.get('Content-Type') == compressed_headers.get('Content-Type')
                        assert data == original
                headers, fallback = fetch('gzip', '/not-a-resource/spa')
                assert gzip.decompress(fallback) == original
                assert headers.get('Cache-Control') == 'no-cache'
                asset = 'vs-icons/Copy.svg'
                headers, icon = fetch('gzip', '/' + asset + '?v=fixture')
                if headers.get('Content-Encoding') == 'gzip':
                    icon = gzip.decompress(icon)
                assert icon == (ROOT / 'web/dist' / asset).read_bytes()
                assert headers.get('Cache-Control') == 'public, max-age=31536000, immutable'

                if args.playwright_module:
                    script = '''
import {pathToFileURL} from 'node:url';
const {chromium}=await import(pathToFileURL(process.env.PROBE_PLAYWRIGHT).href);
const browser=await chromium.launch({channel:'msedge',headless:true});
try {
  const page=await browser.newPage({locale:'zh-CN'});const errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  const response=await page.goto(process.env.PROBE_URL+'/?token=embedded-asset-fixture');
  await page.waitForFunction(()=>document.querySelector('#root')?.children.length>0);
  await page.getByRole('button',{name:'ACECode 快捷菜单'}).waitFor();
  if(response.headers()['content-encoding']!=='gzip'||errors.length)throw Error(JSON.stringify(errors));
  console.log('embedded gzip browser startup: PASS');
} finally {await browser.close();}
'''
                    subprocess.run(['node', '--input-type=module', '-'], input=script,
                                   text=True, encoding='utf-8',
                                   env={**env, 'PROBE_PLAYWRIGHT': args.playwright_module,
                                        'PROBE_URL': url}, check=True, timeout=50)
                print(json.dumps({'http_checks': 8, 'identity_bytes': len(original),
                                  'gzip_bytes': len(compressed),
                                  'sha256': hashlib.sha256(original).hexdigest()}))
            finally:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)


if __name__ == '__main__':
    main()
