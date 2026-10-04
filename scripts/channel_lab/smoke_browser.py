"""Browser acceptance for a running Channel Lab. Uses optional Python Playwright."""
import argparse
import json
import re
from pathlib import Path
import time
from playwright.sync_api import sync_playwright, expect


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--url', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--no-screenshots', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(channel='msedge', headless=True)
        context = browser.new_context(viewport={'width': 1240, 'height': 850}, accept_downloads=True)
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda exc: errors.append(str(exc)))
        page.goto(args.url)
        expect(page.locator('#service-status')).to_have_text('本机服务在线')
        text = f'自聊验证 {int(time.time())}\n中文、多行与 <script> 都是普通文本。'
        page.locator('#message-input').fill(text)
        page.locator('#send').click()
        expect(page.locator('.bubble').filter(has_text=text)).to_have_count(2)
        second = context.new_page()
        second.goto(args.url)
        expect(second.locator('.bubble').filter(has_text=text)).to_have_count(2)
        page.reload()
        expect(page.locator('.bubble').filter(has_text=text)).to_have_count(2)
        with page.expect_download() as downloaded:
            page.locator('#export').click()
        data = json.loads(Path(downloaded.value.path()).read_text(encoding='utf-8'))
        assert len([m for m in data['messages'] if m['text'] == text]) == 2
        help_messages = page.locator('.message').filter(has=page.locator('.message-meta').filter(has_text='IM 帮助'))
        help_count = help_messages.count()
        page.locator('[data-command="/help"]').click()
        expect(help_messages).to_have_count(help_count + 1)
        expect(help_messages.last).to_contain_text('/session search')
        page.reload()
        expect(help_messages).to_have_count(help_count + 1)
        # Enter during CJK composition must not submit.
        page.locator('#message-input').fill('拼音输入中')
        page.locator('#message-input').dispatch_event('keydown', {'key': 'Enter', 'isComposing': True})
        expect(page.locator('#message-input')).to_have_value('拼音输入中')
        page.locator('#message-input').fill('')
        # A delayed send completion must not overwrite a newer draft after changing rooms.
        page.evaluate('''() => {
          window.originalFetch = window.fetch;
          window.fetch = async (...args) => {
            const response = await window.originalFetch(...args);
            if (String(args[0]).endsWith('/api/messages')) {
              await new Promise(resolve => { window.releaseSend = resolve; });
            }
            return response;
          };
        }''')
        page.locator('#message-input').fill('延迟发送回执测试')
        page.locator('#send').click()
        page.wait_for_function('() => typeof window.releaseSend === "function"')
        page.locator('#message-input').fill('这是下一条草稿，不能丢失')
        page.locator('[data-room="acecode"]').click()
        page.evaluate('() => { window.fetch = window.originalFetch; window.releaseSend(); }')
        expect(page.locator('#send')).to_be_enabled()
        page.locator('[data-room="self"]').click()
        expect(page.locator('#message-input')).to_have_value('这是下一条草稿，不能丢失')
        page.locator('#message-input').fill('')
        if not args.no_screenshots:
            page.screenshot(path=str(args.output / 'im-self-desktop.png'), full_page=True)
        page.locator('[data-room="acecode"]').click()
        expect(page.locator('#binding-state')).to_have_text('Channel 已连接', timeout=20000)
        initial_session = page.locator('#session-info').get_attribute('title')
        current_state = page.request.get(args.url + '/api/state?room=acecode').json()
        other_title = 'IM 会话切换测试' if current_state['session_title'] == 'IM Channel 测试' else 'IM Channel 测试'
        catalog_count = page.locator('.bubble').filter(has_text='Use /sessions').count()
        page.locator('[data-command="/session"]').click()
        expect(page.locator('.bubble').filter(has_text='Use /sessions')).to_have_count(catalog_count + 1, timeout=20000)
        catalog = page.locator('.bubble').filter(has_text='Use /sessions').last.inner_text()
        match = re.search(r'^(\d+)\. ' + re.escape(other_title) + r' \|', catalog, re.M)
        assert match, catalog
        page.locator('#message-input').fill('/session ' + match.group(1))
        page.locator('#send').click()
        page.wait_for_function('(original) => document.querySelector("#session-info").title !== original', arg=initial_session, timeout=20000)
        page.locator('#connection').click()
        expect(page.locator('#binding-state')).to_have_text('Channel 未连接', timeout=20000)
        page.locator('#message-input').fill('断线时保留输入')
        page.locator('#send').click()
        expect(page.locator('#error')).to_contain_text('Channel 尚未连接')
        expect(page.locator('#message-input')).to_have_value('断线时保留输入')
        help_count = help_messages.count()
        page.locator('#message-input').fill('/help')
        page.locator('#send').click()
        expect(help_messages).to_have_count(help_count + 1)
        expect(help_messages.last).to_contain_text('/aq --cancel')
        expect(page.locator('#error')).to_be_hidden()
        page.locator('#message-input').fill('')
        page.locator('#connection').click()
        expect(page.locator('#binding-state')).to_have_text('Channel 已连接', timeout=20000)
        page.locator('[data-command="/aq --status"]').click()
        page.wait_for_timeout(1800)
        if not args.no_screenshots:
            page.screenshot(path=str(args.output / 'im-channel-desktop.png'), full_page=True)
        page.set_viewport_size({'width': 390, 'height': 844})
        if not args.no_screenshots:
            page.screenshot(path=str(args.output / 'im-channel-mobile.png'), full_page=True)
        assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth')
        assert not errors, errors
        print(json.dumps({'self_echo': True, 'multi_window': True, 'history_export': True,
                          'ime': True, 'draft_preserved': True, 'local_help': True,
                          'disconnected_help': True, 'session_switch': True, 'disconnect_reconnect': True,
                          'mobile_overflow': False, 'page_errors': errors}, ensure_ascii=False))
        browser.close()


if __name__ == '__main__':
    main()
