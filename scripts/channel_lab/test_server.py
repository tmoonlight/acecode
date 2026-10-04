import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError

from server import Lab, ApiError, make_server, request_json, write_json


class LabTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.lab = Lab(Path(self.temp.name))
        self.server = make_server(self.lab)
        self.worker = threading.Thread(target=self.server.serve_forever)
        self.worker.start()

    def tearDown(self):
        self.server.shutdown()
        self.worker.join()
        self.server.server_close()
        self.lab.db.close()
        self.temp.cleanup()

    def activate(self, sid='s1', token='private-inbound'):
        return self.lab.lifecycle({'type': 'channel.activate', 'protocol_version': 1,
                                   'session_id': sid, 'inbound': {
                                       'url': 'http://127.0.0.1:28999/rc/send', 'token': token}})

    def test_self_chat_persistence_dedup_and_collision(self):
        payload = {'room': 'self', 'text': '你好\n第二行 <script>', 'client_id': 'm1'}
        first = request_json(self.lab.url + '/api/messages', payload)
        self.assertEqual(first['status'], 'sent')
        self.assertEqual(request_json(self.lab.url + '/api/messages', payload)['id'], first['id'])
        self.assertEqual([m['sender'] for m in self.lab.history('self')], ['me', 'echo'])
        reopened = Lab(Path(self.temp.name))
        self.assertEqual(reopened.history('self')[1]['text'], payload['text'])
        reopened.db.close()
        with self.assertRaises(HTTPError) as error:
            request_json(self.lab.url + '/api/messages', {**payload, 'text': 'different'})
        self.assertEqual(error.exception.code, 409)

    def test_help_is_local_persistent_and_idempotent_in_both_rooms(self):
        with patch('server.request_json') as upstream:
            for room in ('self', 'acecode'):
                payload = {'room': room, 'text': '  /help\n', 'client_id': 'help-request'}
                first = self.lab.send(payload)
                self.assertTrue(first['ok'])
                self.assertEqual(self.lab.send(payload)['id'], first['id'])
                history = self.lab.history(room)
                self.assertEqual([m['sender'] for m in history], ['me', 'system'])
                self.assertEqual(history[-1]['kind'], 'help')
                for command in ('/session search', '/session 2', '/aq --repeat', '/aq --back', '/aq --cancel'):
                    self.assertIn(command, history[-1]['text'])
            # Connected help also bypasses the upstream agent and its input queue.
            self.activate()
            self.lab.send({'room': 'acecode', 'text': '/help', 'client_id': 'connected-help'})
            upstream.assert_not_called()
        reopened = Lab(Path(self.temp.name))
        self.assertEqual(reopened.history('acecode')[-1]['kind'], 'help')
        reopened.db.close()

    def test_help_only_intercepts_the_complete_command(self):
        payload = {'room': 'acecode', 'text': '/help me with code', 'client_id': 'ordinary'}
        with self.assertRaises(ApiError):
            self.lab.send(payload)
        self.activate()
        with patch('server.request_json', return_value={'ok': True}) as upstream:
            self.assertTrue(self.lab.send(payload)['ok'])
            self.assertEqual(upstream.call_args.args[1]['text'], payload['text'])

    def test_binding_idempotence_stale_cleanup_and_webhook_dedup(self):
        first = self.activate()
        repeated = self.activate()
        self.assertTrue(repeated['already_running'])
        self.assertEqual(first['binding_token'], repeated['binding_token'])
        second = self.activate('s2')
        self.lab.lifecycle({'type': 'channel.deactivate', 'protocol_version': 1,
                            'session_id': 's1', 'binding_token': first['binding_token']})
        self.assertEqual(self.lab.state('acecode')['session_id'], 's2')
        output = {'type': 'assistant_message', 'session_id': 's2', 'seq': 3, 'text': '实际回复'}
        request_json(second['outbound']['url'], output)
        request_json(second['outbound']['url'], output)
        self.assertEqual(len(self.lab.history('acecode')), 1)
        with self.assertRaises(HTTPError) as error:
            request_json(first['outbound']['url'], {**output, 'session_id': 's1'})
        self.assertEqual(error.exception.code, 403)
        exported = request_json(self.lab.url + '/api/export?room=acecode')
        self.assertNotIn('binding_token', json.dumps(exported))
        for secret in ('private-inbound', first['binding_token'], second['binding_token']):
            self.assertNotIn(secret.encode(), (Path(self.temp.name) / 'history.sqlite3').read_bytes())
        self.lab.lifecycle({'type': 'channel.deactivate', 'protocol_version': 1,
                            'session_id': 's2', 'binding_token': second['binding_token']})
        self.assertFalse(self.lab.state('acecode')['connected'])

    def test_stdio_plugin_and_auth(self):
        descriptor = Path(self.temp.name) / 'plugin.json'
        write_json(descriptor, {'url': self.lab.url, 'token': self.lab.admin_token})
        payload = {'type': 'channel.activate', 'protocol_version': 1, 'session_id': 'stdio',
                   'inbound': {'url': 'http://127.0.0.1:28999/rc/send', 'token': 'test-secret'}}
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('plugin.py')), str(descriptor)],
                                input=json.dumps(payload).encode(), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['state'], 'connected')
        self.assertEqual(result.stderr, b'')
        with self.assertRaises(HTTPError) as error:
            request_json(self.lab.url + '/admin/lifecycle', payload)
        self.assertEqual(error.exception.code, 403)

    def test_input_validation_and_origin_host_boundary(self):
        for text in ('', ' \n ', '中' * 6000, 123):
            with self.assertRaises(HTTPError) as error:
                request_json(self.lab.url + '/api/messages', {'room': 'self', 'text': text, 'client_id': 'x'})
            self.assertEqual(error.exception.code, 400)
        for headers in ({'Origin': 'https://example.com'}, {'Origin': 'null'},
                        {'Host': 'example.com'}, {'Sec-Fetch-Site': 'cross-site'}):
            with self.assertRaises(HTTPError) as error:
                request_json(self.lab.url + '/api/messages',
                             {'room': 'self', 'text': 'x', 'client_id': 'x'}, headers)
            self.assertEqual(error.exception.code, 403)
        self.assertEqual(self.lab.history('self'), [])

    def test_disconnected_and_unknown_delivery_never_auto_retry(self):
        payload = {'room': 'acecode', 'text': '消息', 'client_id': 'unknown'}
        with self.assertRaises(ApiError):
            self.lab.send(payload)
        self.assertEqual(self.lab.history('acecode'), [])
        self.activate()
        with patch('server.request_json', side_effect=URLError('network down')) as upstream:
            result = self.lab.send(payload)
            self.assertEqual(result['status'], 'unknown')
            self.assertFalse(result['ok'])
            self.assertFalse(self.lab.send(payload)['ok'])
            self.assertEqual(upstream.call_count, 1)

    def test_stale_token_cannot_detach_new_binding_of_same_session(self):
        first = self.activate('same', 'first-inbound')
        second = self.activate('same', 'second-inbound')
        self.lab.lifecycle({'type': 'channel.deactivate', 'protocol_version': 1,
                            'session_id': 'same', 'binding_token': first['binding_token']})
        self.assertTrue(self.lab.state('acecode')['connected'])
        self.assertEqual(self.lab.binding['binding_token'], second['binding_token'])

    def test_actual_inbound_http_hop_and_webhook_return(self):
        # A second real HTTP server plays only the upstream transport endpoint.
        from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
        output = self.activate()
        observed = []
        lab = self.lab

        class Upstream(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                observed.append((self.path, self.headers['X-ACECode-RC-Token'],
                                 json.loads(self.rfile.read(int(self.headers['Content-Length'])))))
                request_json(output['outbound']['url'], {
                    'type': 'assistant_message', 'session_id': 's1', 'seq': 1, 'text': 'mock reply'})
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"ok":true}')

        upstream = ThreadingHTTPServer(('127.0.0.1', 0), Upstream)
        worker = threading.Thread(target=upstream.serve_forever)
        worker.start()
        try:
            lab.binding['url'] = f'http://127.0.0.1:{upstream.server_port}/rc/send'
            result = lab.send({'room': 'acecode', 'text': '/session', 'client_id': 'http-test'})
            self.assertTrue(result['ok'])
            self.assertEqual(observed[0][0], '/rc/send')
            self.assertEqual(observed[0][1], 'private-inbound')
            self.assertEqual(observed[0][2]['text'], '/session')
            self.assertEqual(lab.history('acecode')[-1]['text'], 'mock reply')
        finally:
            upstream.shutdown()
            worker.join()
            upstream.server_close()


if __name__ == '__main__':
    unittest.main()
