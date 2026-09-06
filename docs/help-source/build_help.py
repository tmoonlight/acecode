"""Generate complete standalone HTML pages; no build step is needed by readers."""
from pathlib import Path
from html import escape
import importlib
import json
import re
from common import icon, plain_text, figure_inventory
from screenshot_assets import Screenshots

SOURCE = Path(__file__).resolve().parent
DEST = SOURCE.parent / "help"
TREE = json.loads((SOURCE / "tree.json").read_text(encoding="utf-8"))


def write(path, value):
    path.write_text(value.rstrip() + "\n", encoding="utf-8", newline="\n")


def build():
    screenshots = Screenshots()
    contents = {}
    for group in range(1, 8):
        module = importlib.import_module(f"group{group}")
        duplicate = set(contents) & set(module.PAGES)
        assert not duplicate, duplicate
        contents.update(module.PAGES)
    articles = [{**entry, "group": group["title"], "group_index": index} for index, group in enumerate(TREE) for entry in group["pages"]]
    assert len(articles) == 49 and set(contents) == {entry["slug"] for entry in articles}
    for article in articles:
        content = contents[article["slug"]]
        if "sections" in article:
            assert article["sections"] == [s["title"] for s in content["sections"]], article["slug"]
        assert content["sources"] and len(plain_text(content["intro"] + " ".join(s["html"] for s in content["sections"]))) >= 280, article["slug"]
        for source in content["sources"]:
            assert (SOURCE.parent.parent / source).exists(), (article["slug"], source)

    shell = (SOURCE / "shell.html").read_text(encoding="utf-8")
    search = []
    source_records = []
    images = []
    for pos, article in enumerate(articles):
        slug, title = article["slug"], article["title"]
        content = contents[slug]
        navigation = []
        for group in TREE:
            rows = []
            for entry in group["pages"]:
                current = entry["slug"] == slug
                href = entry["slug"] + ".html"
                link = f'<a href="{href}"' + (' aria-current="page"' if current else '') + f'>{escape(entry["title"])}</a>'
                if "sections" in entry:
                    children = "".join(f'<li><a href="{href}#{s["id"]}"' + (' data-toc-link' if current else '') + f'>{escape(s["title"])}</a></li>' for s in contents[entry["slug"]]["sections"])
                    rows.append(f'<li><details class="nav-article"{" open" if current else ""}><summary>{link}{icon("chevron")}</summary><ul class="nav-section-list">{children}</ul></details></li>')
                else:
                    rows.append(f'<li>{link}</li>')
            navigation.append(f'<details class="nav-group"{" open" if group["title"] == article["group"] else ""}><summary><span>{escape(group["title"])}</span>{icon("chevron")}</summary><ul>{"".join(rows)}</ul></details>')
        toc = "".join(f'<li><a href="#{s["id"]}" data-toc-link>{escape(s["title"])}</a></li>' for s in content["sections"])
        body = content["intro"] + "\n" + "\n".join(f'<section aria-labelledby="{s["id"]}"><h2 id="{s["id"]}">{escape(s["title"])}</h2>{s["html"]}</section>' for s in content["sections"])
        figures = figure_inventory(body)
        body = screenshots.render(body, figures)
        pager = []
        for offset, label, classname in [(-1, "上一篇", "previous"), (1, "下一篇", "next")]:
            target = pos + offset
            if 0 <= target < len(articles):
                other = articles[target]
                caption = icon("back") + escape(other["title"]) if offset < 0 else escape(other["title"]) + icon("arrow")
                pager.append(f'<a class="page-link {classname}" href="{other["slug"]}.html"><span>{label}</span><strong>{caption}</strong></a>')
            else:
                pager.append('<span class="pager-start">' + ('文档首页' if offset < 0 else '已到文档末尾') + '</span>')
        values = {"TITLE": escape(title), "LEAD": escape(content["lead"]), "GROUP": escape(article["group"]), "NAVIGATION": "\n".join(navigation), "TOC": toc, "BODY": body, "PAGER": "".join(pager)}
        html = re.sub(r"\{\{([A-Z]+)\}\}", lambda match: values[match[1]], shell)
        write(DEST / (slug + ".html"), html)
        full_text = plain_text(body)
        search.append({"page": title, "group": article["group"], "title": title, "url": slug + ".html", "description": content["lead"], "text": full_text})
        for s in content["sections"]:
            section_html = re.search(r'<section aria-labelledby="' + re.escape(s["id"]) + r'">(.*?)</section>', body, re.S)
            assert section_html, s["id"]
            text = plain_text(section_html[1])
            search.append({"page": title, "group": article["group"], "title": s["title"], "url": slug + ".html#" + s["id"], "description": text[:125], "text": text})
        source_records.append({"article": title, "file": slug + ".html", "sources": content["sources"]})
        images.extend({"article": title, "file": slug + ".html", **f} for f in figures)
    write(DEST / "assets/search-index.js", "// Generated from authored documentation; all content remains local.\nwindow.ACECODE_HELP_INDEX = " + json.dumps(search, ensure_ascii=False, indent=2) + ";")
    write(SOURCE / "sources.json", json.dumps(source_records, ensure_ascii=False, indent=2))
    assert len({f["id"] for f in images}) == len(images)
    screenshots.validate()
    write(SOURCE / "images.json", json.dumps(images, ensure_ascii=False, indent=2))
    captured = sum(f["status"] == "captured" for f in images)
    rows = ["# 帮助文档配图计划", "", f"由 build_help.py 自动生成。共 {len(images)} 处配图，{captured} 处已接入 Windows 实拍，{len(images) - captured} 处保留占位。已接入的 {len(screenshots.captures)} 张原图、时间、应用版本和 SHA-256 记录在 images.json。部分图组尚缺辅助画面，见下表。", "", "| 编号 | 文章 | 状态 | 画面 | 配图说明 | 待补内容或原因 |", "| --- | --- | --- | --- | --- | --- |"]
    for f in images:
        status = "已配图，辅助画面待补" if f.get("pending_details") else ("已实拍" if f["status"] == "captured" else "占位")
        pending = f.get("pending_details", f.get("pending", {}).get("note", ""))
        rows.append(f'| {f["id"]} | [{f["article"]}](../help/{f["file"]}) | {status} | {f["title"]} | {f["description"].replace("|", "／")} | {pending.replace("|", "／")} |')
    write(SOURCE / "image-plan.md", "\n".join(rows))
    print(f"Generated {len(articles)} articles in {len(TREE)} groups, {len(search)} search entries, {captured} captured figures, {len(images) - captured} image placeholders.")


if __name__ == "__main__":
    build()
