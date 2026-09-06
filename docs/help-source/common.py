"""Authoring helpers. These files are not shipped in the offline HTML package."""
from html import escape
from html.parser import HTMLParser


def icon(name):
    paths = {
        "arrow": '<path d="M4 12h15m-6-6 6 6-6 6"/>',
        "back": '<path d="M20 12H5m6-6-6 6 6 6"/>',
        "chevron": '<path d="m8 5 7 7-7 7"/>',
        "copy": '<path d="M8 8h12v13H8zM16 8V3H3v13h5"/>',
    }
    extra = " group-arrow" if name == "chevron" else ""
    return f'<svg class="icon{extra}" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true">{paths[name]}</svg>'


def code(value, label="终端"):
    return f'<div class="code-block"><div class="code-heading"><span>{escape(label)}</span><button class="copy-button js-only" type="button" aria-label="复制代码">{icon("copy")}<span>复制</span></button></div><pre><code>{escape(value.strip())}</code></pre></div>'


def note(title, body, warning=False):
    return f'<aside class="note{" warning" if warning else ""}"><strong>{escape(title)}</strong><p>{body}</p></aside>'


def figure(identifier, title, description):
    return f'<figure class="screenshot" data-placeholder="{escape(identifier)}"><div class="image-placeholder" role="img" aria-label="图片占位：{escape(title)}"><span class="placeholder-id">{escape(identifier)}</span><div class="placeholder-content"><span class="placeholder-mark" aria-hidden="true"></span><span class="placeholder-kind">图片占位</span><strong>{escape(title)}</strong><span>此处补充实际界面截图</span></div><span class="placeholder-ratio" aria-hidden="true">16 : 9</span></div><figcaption>{escape(description)}</figcaption></figure>'


def table(headers, rows):
    head = "".join(f'<th scope="col">{v}</th>' for v in headers)
    body = "".join("<tr>" + "".join(f"<td>{v}</td>" for v in row) + "</tr>" for row in rows)
    return f'<div class="table-scroll"><table><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table></div>'


def section(identifier, title, *body):
    return {"id": identifier, "title": title, "html": "\n".join(body)}


def page(lead, sections, sources, intro=""):
    return {"lead": lead, "sections": sections, "sources": sources, "intro": intro}


class PlainText(HTMLParser):
    def __init__(self):
        super().__init__()
        self.parts = []

    def handle_data(self, data):
        self.parts.append(data)


def plain_text(html):
    parser = PlainText()
    parser.feed(html)
    return " ".join(" ".join(parser.parts).split())


class FigureInventory(HTMLParser):
    def __init__(self, html):
        super().__init__(convert_charrefs=True)
        self.figures = []
        self.current = None
        self.capture = None
        self.feed(html)

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        if tag == "figure" and "data-placeholder" in attributes:
            self.current = {"id": attributes["data-placeholder"], "title": "", "description": ""}
            self.figures.append(self.current)
        if self.current is not None and tag in ("strong", "figcaption"):
            self.capture = "title" if tag == "strong" else "description"

    def handle_data(self, data):
        if self.current is not None and self.capture:
            self.current[self.capture] += data

    def handle_endtag(self, tag):
        if tag in ("strong", "figcaption"):
            self.capture = None
        if tag == "figure":
            self.current = None


def figure_inventory(html):
    return FigureInventory(html).figures
