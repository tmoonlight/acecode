"""Verify search navigation against synthetic data, including legacy ordinal links."""
import argparse
import json
from pathlib import Path
from urllib.parse import urlencode
from measure import daemon
from playwright.sync_api import sync_playwright

def verify(executable, profile, static_dir):
    manifest = json.loads((profile / "manifest.json").read_text(encoding="utf-8"))
    sid = manifest["sessions"][0]["id"]
    paths = list((profile / ".acecode" / "projects").rglob(sid + ".jsonl"))
    if len(paths) != 1:
        raise ValueError("Expected one synthetic session")
    offset = 0
    users = 0
    target = None
    with paths[0].open("rb") as history:
        for ordinal, line in enumerate(history):
            record = json.loads(line)
            if record.get("role") == "user" and not record.get("is_meta") and not record.get("metadata", {}).get("hidden_goal_context"):
                users += 1
                if users == 5:
                    target = {"offset": str(offset), "ordinal": ordinal, "content": record["content"]}
                    break
            offset += len(line)
    assert target
    results = []
    with daemon(executable, profile, static_dir) as (base, token, _):
        with sync_playwright() as pw:
            browser = pw.chromium.launch(headless=True)
            for key, value in (("message_position", target["offset"]), ("message_ordinal", target["ordinal"])):
                page = browser.new_page(viewport={"width": 1440, "height": 1000})
                errors = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                params = {"token": token, "open": sid, "workspace": manifest["project_hash"], key: value}
                page.goto(base + "/?" + urlencode(params), wait_until="domcontentloaded")
                selector = '[data-chat-user-message="true"][data-chat-message-position="' + target["offset"] + '"]'
                page.locator(selector).wait_for(state="visible", timeout=120000)
                page.wait_for_timeout(1200)
                row = page.locator(selector)
                assert target["content"] in row.inner_text()
                geometry = row.evaluate("""row => {
                  const container = row.closest('.ace-chat-transcript-scroll');
                  const r = row.getBoundingClientRect(), c = container.getBoundingClientRect();
                  return {visible: r.bottom > c.top && r.top < c.bottom, top: r.top, bottom: r.bottom};
                }""")
                assert geometry["visible"], geometry
                resources = page.evaluate("""performance.getEntriesByType('resource')
                  .filter(e => e.name.includes('/messages?'))
                  .map(e => new URL(e.name).search)""")
                results.append({"mode": key, "target_ordinal": target["ordinal"], "target_position": target["offset"],
                                "correct_row": True, "geometry": geometry, "queries": resources, "errors": errors})
                assert not errors
                page.close()
            browser.close()
    return results

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--static-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = verify(args.executable, args.profile, args.static_dir)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
