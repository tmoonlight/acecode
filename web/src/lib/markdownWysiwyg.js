import { fromMarkdown } from 'mdast-util-from-markdown';
import { gfmFromMarkdown, gfmToMarkdown } from 'mdast-util-gfm';
import { mathFromMarkdown } from 'mdast-util-math';
import { toMarkdown } from 'mdast-util-to-markdown';
import { gfm } from 'micromark-extension-gfm';
import { math } from 'micromark-extension-math';

const EMPTY_TEXT = () => ({ text: '' });

function rawSource(node, source) {
  const start = Number(node?.position?.start?.offset);
  const end = Number(node?.position?.end?.offset);
  return Number.isFinite(start) && Number.isFinite(end) && end >= start
    ? source.slice(start, end)
    : '';
}

function withText(children) {
  return Array.isArray(children) && children.length > 0 ? children : [EMPTY_TEXT()];
}

function opaqueInline(node, source) {
  return {
    type: 'opaque-inline',
    raw: rawSource(node, source),
    sourceType: String(node?.type || 'unknown'),
    children: [EMPTY_TEXT()],
  };
}

function opaqueBlock(node, source) {
  return {
    type: 'opaque-block',
    raw: rawSource(node, source),
    sourceType: String(node?.type || 'unknown'),
    children: [EMPTY_TEXT()],
  };
}

function inlineNodes(nodes, source, marks = {}) {
  const out = [];
  for (const node of Array.isArray(nodes) ? nodes : []) {
    if (!node) continue;
    if (node.type === 'text') {
      out.push({ text: String(node.value || ''), ...marks });
    } else if (node.type === 'strong') {
      out.push(...inlineNodes(node.children, source, { ...marks, bold: true }));
    } else if (node.type === 'emphasis') {
      out.push(...inlineNodes(node.children, source, { ...marks, italic: true }));
    } else if (node.type === 'delete') {
      out.push(...inlineNodes(node.children, source, { ...marks, strike: true }));
    } else if (node.type === 'inlineCode') {
      out.push({ text: String(node.value || ''), ...marks, code: true });
    } else if (node.type === 'link') {
      out.push({
        type: 'link',
        url: String(node.url || ''),
        title: node.title == null ? null : String(node.title),
        children: withText(inlineNodes(node.children, source, marks)),
      });
    } else if (node.type === 'break') {
      out.push({ type: 'hard-break', children: [EMPTY_TEXT()] });
    } else {
      out.push(opaqueInline(node, source));
    }
  }
  return withText(out);
}

function blockNode(node, source) {
  switch (node?.type) {
    case 'paragraph':
      return { type: 'paragraph', children: inlineNodes(node.children, source) };
    case 'heading':
      return {
        type: 'heading',
        level: Math.min(6, Math.max(1, Number(node.depth) || 1)),
        children: inlineNodes(node.children, source),
      };
    case 'blockquote':
      return {
        type: 'blockquote',
        children: withText(blockNodes(node.children, source)),
      };
    case 'code':
      if (String(node.lang || '').toLowerCase() === 'mermaid'
          || String(node.lang || '').toLowerCase() === 'math') {
        return opaqueBlock(node, source);
      }
      return {
        type: 'code-block',
        lang: node.lang == null ? '' : String(node.lang),
        meta: node.meta == null ? '' : String(node.meta),
        children: [{ text: String(node.value || '') }],
      };
    case 'list':
      return {
        type: 'list',
        ordered: node.ordered === true,
        start: Number.isFinite(node.start) ? node.start : null,
        spread: node.spread === true,
        children: (node.children || []).map((child) => blockNode(child, source)),
      };
    case 'listItem':
      return {
        type: 'list-item',
        checked: typeof node.checked === 'boolean' ? node.checked : null,
        spread: node.spread === true,
        children: withText(blockNodes(node.children, source)),
      };
    case 'thematicBreak':
      return { type: 'thematic-break', children: [EMPTY_TEXT()] };
    case 'table':
      return {
        type: 'table',
        align: Array.isArray(node.align) ? node.align : [],
        children: (node.children || []).map((row) => ({
          type: 'table-row',
          children: (row.children || []).map((cell) => ({
            type: 'table-cell',
            children: [{ type: 'paragraph', children: inlineNodes(cell.children, source) }],
          })),
        })),
      };
    default:
      return opaqueBlock(node, source);
  }
}

function blockNodes(nodes, source) {
  return (Array.isArray(nodes) ? nodes : []).map((node) => blockNode(node, source));
}

export function markdownToSlate(source) {
  const text = String(source || '');
  const root = fromMarkdown(text, {
    extensions: [gfm(), math()],
    mdastExtensions: [gfmFromMarkdown(), mathFromMarkdown()],
  });
  const children = blockNodes(root.children, text);
  return children.length > 0 ? children : [{ type: 'paragraph', children: [EMPTY_TEXT()] }];
}

function markedTextNode(node) {
  const value = String(node?.text || '');
  let result = node?.code
    ? { type: 'inlineCode', value }
    : { type: 'text', value };
  if (node?.bold) result = { type: 'strong', children: [result] };
  if (node?.italic) result = { type: 'emphasis', children: [result] };
  if (node?.strike) result = { type: 'delete', children: [result] };
  return result;
}

function inlineMdast(nodes) {
  const out = [];
  for (const node of Array.isArray(nodes) ? nodes : []) {
    if (node && Object.prototype.hasOwnProperty.call(node, 'text')) {
      out.push(markedTextNode(node));
    } else if (node?.type === 'link') {
      out.push({
        type: 'link',
        url: String(node.url || ''),
        title: node.title || null,
        children: inlineMdast(node.children),
      });
    } else if (node?.type === 'hard-break') {
      out.push({ type: 'break' });
    } else if (node?.type === 'opaque-inline') {
      out.push({ type: 'opaqueInline', raw: String(node.raw || '') });
    }
  }
  return out.length > 0 ? out : [{ type: 'text', value: '' }];
}

function blockMdast(node) {
  switch (node?.type) {
    case 'paragraph':
      return { type: 'paragraph', children: inlineMdast(node.children) };
    case 'heading':
      return {
        type: 'heading',
        depth: Math.min(6, Math.max(1, Number(node.level) || 1)),
        children: inlineMdast(node.children),
      };
    case 'blockquote':
      return { type: 'blockquote', children: blockMdastChildren(node.children) };
    case 'code-block':
      return {
        type: 'code',
        lang: node.lang || null,
        meta: node.meta || null,
        value: (node.children || []).map((child) => child.text || '').join(''),
      };
    case 'list':
      return {
        type: 'list',
        ordered: node.ordered === true,
        start: node.ordered && Number.isFinite(node.start) ? node.start : null,
        spread: node.spread === true,
        children: (node.children || []).map(blockMdast),
      };
    case 'list-item':
      return {
        type: 'listItem',
        checked: typeof node.checked === 'boolean' ? node.checked : null,
        spread: node.spread === true,
        children: blockMdastChildren(node.children),
      };
    case 'thematic-break':
      return { type: 'thematicBreak' };
    case 'table':
      return {
        type: 'table',
        align: Array.isArray(node.align) ? node.align : [],
        children: (node.children || []).map((row) => ({
          type: 'tableRow',
          children: (row.children || []).map((cell) => {
            const paragraphs = (cell.children || [])
              .filter((child) => child?.type === 'paragraph');
            const inline = paragraphs.flatMap((paragraph, index) => [
              ...(index > 0 ? [{ type: 'break' }] : []),
              ...inlineMdast(paragraph.children),
            ]);
            return { type: 'tableCell', children: inline.length ? inline : [{ type: 'text', value: '' }] };
          }),
        })),
      };
    case 'opaque-block':
      return { type: 'opaqueBlock', raw: String(node.raw || '') };
    default:
      return { type: 'paragraph', children: inlineMdast(node?.children) };
  }
}

function blockMdastChildren(nodes) {
  return (Array.isArray(nodes) ? nodes : []).map(blockMdast);
}

const opaqueHandlers = {
  opaqueBlock(node) {
    return String(node.raw || '');
  },
  opaqueInline(node) {
    return String(node.raw || '');
  },
};

export function slateToMarkdown(document) {
  const nodes = Array.isArray(document) && document.length > 0
    ? document
    : [{ type: 'paragraph', children: [EMPTY_TEXT()] }];
  return toMarkdown(
    { type: 'root', children: nodes.map(blockMdast) },
    {
      bullet: '-',
      emphasis: '_',
      strong: '*',
      fence: '`',
      extensions: [
        gfmToMarkdown(),
        { handlers: opaqueHandlers },
      ],
    },
  );
}

export function markdownRoundTrip(source) {
  return slateToMarkdown(markdownToSlate(source));
}
