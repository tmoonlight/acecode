// Mermaid 源码的检查与改写(纯字符串逻辑,不碰 DOM,也不加载 mermaid)。
//
// 原则:只在 Mermaid 当成语法处理的位置做检查,标签里的文字一律当文字。能把有风险的写法拿掉
// 而图还成立的,改写后照画(指令、front matter、点击 / 链接语句、HTML 标签、HTML 实体);
// 拿不掉的才拒绝(样式里的 url()、图片形状)。画出来的 SVG 还要过 sanitizeMermaidSvg,并且只以
// <img> 显示,这里不是唯一的一道。
//
// 两条不能松的线:
// 1. 交给 Mermaid 的文本里不能有 HTML 标签(`<br>` 除外),也不能有会让它在绘制过程中向外取
//    资源的语句:绘制是在当前页面里进行的,带地址的图片会当场发出请求。
// 2. 合法的图交给 Mermaid 的文本一个字都不改。

export const MAX_MERMAID_SOURCE_BYTES = 64 * 1024;
export const MAX_MERMAID_SOURCE_LINES = 1000;

const SAFE_MERMAID_LABEL_BREAK = /<br[ \t]*\/?>/gi;

// 标签里像 HTML 标签的文字(`<cachePath>`、`List<String>`、`<img src=x>`)交给 Mermaid 之前改写成
// 它自己的实体码 `#lt;` / `#gt;`:Mermaid 拿到的源码里除了 `<br>` 不再有能被当成标签的 `<`,图上
// 照原样显示尖括号。能配成一对的 `<...>` 连右尖括号一起改(flowchart 会吞掉标签里裸的 `>`);
// 右尖括号前是 `-` `=` `.` 的不算一对,那多半是连线箭头 `-->`。构建会拒绝 lookbehind,只能用前瞻写。
const MERMAID_MARKUP_TOKEN = /<br[ \t]*\/?>|<(\/?[A-Za-z](?:[^<>[\](){}|\r\n]*[^-=.<>[\](){}|\r\n])?)>|<(?=\/?[A-Za-z])/gi;

// 类图的 `<<interface>>`、状态图的 `<<choice>>` 是语法不是标签,改写了就画成一行普通文字 / 一个
// 普通状态。只认整行就是这一个标记的写法(后面最多跟一个类名),里面只许字母数字下划线,
// 带不了属性,也夹带不了别的内容。
const CLASS_STEREOTYPE_LINE = /^[ \t]*<<[A-Za-z][A-Za-z0-9_]*>>(?:[ \t]+[\w\u0080-￿.`~-]+)?[ \t]*$/;
const STATE_STEREOTYPE_LINE = /^[ \t]*state[ \t]+[^\s<>"]+[ \t]+<<(?:choice|fork|join)>>[ \t]*$/i;

function isStereotypeLine(line, family) {
  if (family === 'class') return CLASS_STEREOTYPE_LINE.test(line);
  if (family === 'state') return STATE_STEREOTYPE_LINE.test(line);
  return false;
}

export function neutralizeMermaidMarkup(source, family = '') {
  return String(source || '').split('\n').map((line) => {
    if (isStereotypeLine(line, family)) return line;
    return line.replace(MERMAID_MARKUP_TOKEN, (match, inner) => {
      if (inner !== undefined) return `#lt;${inner}#gt;`;
      return match === '<' ? '#lt;' : match;
    });
  }).join('\n');
}

// HTML 实体(`&lt;`、`&#60;`、`&#x3c;`、`&amp;`)换成 Mermaid 自己的写法(`#lt;`、`#60;`、`#amp;`):
// 效果与上面改写原文尖括号完全一样,图上显示的是字符本身。十六进制的换成十进制,Mermaid 只认十进制。
const HTML_ENTITY = /&(#x[0-9a-f]+|#\d+|[a-z][a-z0-9]*);/gi;

export function normalizeMermaidEntities(source) {
  return String(source || '').replace(HTML_ENTITY, (match, body) => {
    if (body[0] !== '#') return `#${body};`;
    if (body[1] !== 'x' && body[1] !== 'X') return `${body};`;
    const code = Number.parseInt(body.slice(2), 16);
    return Number.isFinite(code) ? `#${code};` : match;
  });
}

// 时序图的消息、注释和 loop / alt 这类分组标题里,`;` 被 Mermaid 当成换行(后半句成了一条语法错误
// 的新语句,整张图画不出来),`#` 之后的内容被当成注释悄悄截掉。这几处文字里的 `;` `#` 改写成实体码,
// 已经是实体码的(`#59;`、`#lt;`)原样保留。
const SEQUENCE_BLOCK_STATEMENT = /^(\s*(?:loop|alt|else|opt|par|and|critical|option|break)\b)(.*)$/i;
const SEQUENCE_TEXT_HEAD = /^\s*note\b|--?[>x)|\\/]/i;
// Mermaid 预处理会把同一行里「style…:…#…;」(classDef 同理)的最后一个分号删掉,给样式里的颜色值
// 让路,行里插进去的实体码会被它吃掉分号。时序图行尾多一个 `;` 只是一条空语句,先垫一个给它删。
const MERMAID_STYLE_SEMICOLON_STRIP = /(?:style|classDef).*:\S*#.*;/;

function escapeSequenceText(text) {
  return text.replace(/#\w+;|[#;]/g, (match) => {
    if (match === '#') return '#35;';
    if (match === ';') return '#59;';
    return match;
  });
}

export function escapeMermaidSequenceText(source) {
  return String(source || '').split('\n').map((line) => {
    let head;
    let text;
    const block = line.match(SEQUENCE_BLOCK_STATEMENT);
    if (block) {
      [, head, text] = block;
    } else {
      const colon = line.indexOf(':');
      if (colon < 0 || !SEQUENCE_TEXT_HEAD.test(line.slice(0, colon))) return line;
      head = line.slice(0, colon + 1);
      text = line.slice(colon + 1);
    }
    const escaped = head + escapeSequenceText(text);
    return MERMAID_STYLE_SEMICOLON_STRIP.test(escaped) ? `${escaped};` : escaped;
  }).join('\n');
}

// 指令(`%%{init: ...}%%`)和开头的 front matter 只用来改主题和配置,拿掉后图照画,用的是界面
// 自己的主题。按行数补回空行,Mermaid 报错的行号才对得上原文。收不了尾的 front matter 返回 null。
function blankLines(text) {
  return '\n'.repeat(text.split('\n').length - 1);
}

export function stripMermaidPreamble(source) {
  let text = String(source || '');
  // 拿掉一段之后,剩下的字符可能又拼出一段新的,所以转到不再变化为止。
  for (let round = 0; round < 8; round += 1) {
    const before = text;
    const open = /^\s*---[ \t]*\n/.exec(text);
    if (open) {
      const rest = text.slice(open[0].length);
      const close = /(?:^|\n)---[ \t]*(?:\n|$)/.exec(rest);
      if (!close) return null;
      const removed = text.slice(0, open[0].length + close.index + close[0].length);
      text = blankLines(removed) + rest.slice(close.index + close[0].length);
    }
    text = text
      .replace(/%%\s*\{[\s\S]*?\}\s*%%/g, blankLines)
      .replace(/%%\s*\{[^\n]*/g, '');
    if (text === before) break;
  }
  return text;
}

// 点击、链接、参与者属性这些语句画出来是超链接或带地址的图片。常见写法整句拿掉(留一个空行
// 占位),图本身不受影响。这一步只是为了让图照画,不是安全边界:同一行里还有别的办法接出这类
// 语句(时序图的 `end properties ...`),拦它们靠 inspectMermaidModel 检查解析后的模型。
// - 时序图:`link` / `links` / `properties` / `details` 后面跟「参与者:」的语句。`;` 等于换行,而
//   消息、注释、分组标题里的 `;` 此前已被 escapeMermaidSequenceText 改成实体码,剩下的都是语句
//   分隔,按它切开逐句看。`link->>B: x` 这种把关键字当参与者名用的不动,交给 Mermaid 自己报错。
// - 其它图:行首的 `click`,类图再加 `link` / `callback`(类的成员区里不算,那是成员名)。第二个词
//   是连线符号的不动:那是把 click 当节点 / 状态名在用,同样交给 Mermaid 报错,不悄悄丢一条连线。
const SEQUENCE_INTERACTION = /^\s*(?:links?|properties|details)[ \t]+[^:\n<>]*:/i;
const CLICK_STATEMENT = /^[ \t]*click[ \t]+(?![-.<>*|:=~&])\S/i;
const CLASS_LINK_STATEMENT = /^[ \t]*(?:click|link|callback)[ \t]+(?![-.<>*|:=~&])\S/i;

export function dropMermaidInteractions(source, family) {
  const lines = String(source || '').split('\n');
  if (family === 'sequence') {
    return lines.map((line) => {
      if (!/links?|properties|details/i.test(line)) return line;
      return line.split(';').filter((statement) => !SEQUENCE_INTERACTION.test(statement)).join(';');
    }).join('\n');
  }
  const pattern = family === 'class' ? CLASS_LINK_STATEMENT : CLICK_STATEMENT;
  let depth = 0;
  return lines.map((line) => {
    if (family === 'class') {
      const inside = depth > 0;
      if (/\{[ \t]*$/.test(line)) depth += 1;
      else if (/^[ \t]*\}[ \t]*$/.test(line)) depth = Math.max(0, depth - 1);
      if (inside) return line;
    }
    return pattern.test(line) ? '' : line;
  }).join('\n');
}

// 真正交给 Mermaid 的文本。复制按钮、退回显示和导出仍用原始源码。
// 顺序有讲究:实体和尖括号先改;时序图的文字转义要在「按 `;` 切语句」之前。
export function prepareMermaidRenderSource(source, family) {
  const body = stripMermaidPreamble(String(source || '').replace(/\r\n?/g, '\n'));
  let text = neutralizeMermaidMarkup(normalizeMermaidEntities(body ?? ''), family);
  if (family === 'sequence') text = escapeMermaidSequenceText(text);
  return dropMermaidInteractions(text, family);
}

const FAMILY_HEADERS = [
  { family: 'flowchart', pattern: /^(?:flowchart|graph)(?:\s+(?:TB|TD|BT|RL|LR))?\s*;?(?:\s+%%.*)?$/ },
  { family: 'state', pattern: /^stateDiagram(?:-v2)?\s*;?(?:\s+%%.*)?$/ },
  { family: 'class', pattern: /^classDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'er', pattern: /^erDiagram\s*;?(?:\s+%%.*)?$/ },
  { family: 'sequence', pattern: /^sequenceDiagram\s*;?(?:\s+%%.*)?$/ },
];

function firstMeaningfulLine(text) {
  return String(text || '').split('\n').find((line) => {
    const trimmed = line.trim();
    return trimmed && !/^%%(?!\{)/.test(trimmed);
  })?.trim() || '';
}

export function mermaidFamilyOf(source) {
  const first = firstMeaningfulLine(source);
  return FAMILY_HEADERS.find(({ pattern }) => pattern.test(first))?.family || '';
}

function utf8ByteLength(value) {
  return new TextEncoder().encode(String(value || '')).byteLength;
}

function rejected(reason) {
  return { ok: false, reason };
}

// 样式语句(行首或 `;` 之后的 style / classDef / linkStyle / cssClass,时序图的 rect / box)里的
// url() 和老式脚本化 CSS。标签里的 `getUrl()`、`behavior: retry`、`@Import` 是文字,不在这里拦。
const STYLE_STATEMENT = '(?:^|[\\n;])[ \\t]*(?:style|classDef|linkStyle|cssClass|rect|box)\\b[^\\n]*';
const STYLE_URL = new RegExp(`${STYLE_STATEMENT}url\\s*\\(`, 'i');
const UNSAFE_STYLE = new RegExp(
  `${STYLE_STATEMENT}(?:expression\\s*\\(|-moz-binding\\b|behavior\\s*:|@(?:import|font-face|namespace|supports|media|document|page)\\b)`,
  'i',
);

// `@{ ... }`(新式形状 / 参与者类型)里能写图片地址,绘制时会当场取图,而它的内容是 YAML,键名
// 可以用转义拼出来,靠文本认不全,所以整个写法不支持。流程图里引号内的 `@{` 是标签文字(流程图的
// 引号一定开始一段字符串),不算;其它图类型不区分。
function hasShapeBlock(body, family) {
  const text = family === 'flowchart' ? body.replace(/"[^"\n]*"/g, '""') : body;
  return /@\s*\{/.test(text);
}

export function inspectMermaidSource(value) {
  const source = String(value || '').replace(/\r\n?/g, '\n');
  if (!source.trim()) return rejected('empty');
  if (utf8ByteLength(source) > MAX_MERMAID_SOURCE_BYTES) return rejected('too-large');
  if (source.split('\n').length > MAX_MERMAID_SOURCE_LINES) return rejected('too-many-lines');
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(source)) {
    return rejected('control-character');
  }

  const body = stripMermaidPreamble(source);
  if (body === null) return rejected('frontmatter');
  const family = mermaidFamilyOf(body);
  if (!family) return rejected(firstMeaningfulLine(body) === '---' ? 'frontmatter' : 'unsupported-family');
  if (/%%\s*\{/.test(body)) return rejected('directive');

  if (hasShapeBlock(body, family)) return rejected('generalized-shape');

  // 按改写后的文本自检一遍:除了 `<br>` 和整行的 `<<名字>>` 标记,不能再有标签。
  const neutral = neutralizeMermaidMarkup(normalizeMermaidEntities(body), family)
    .split('\n')
    .filter((line) => !isStereotypeLine(line, family))
    .join('\n')
    .replace(SAFE_MERMAID_LABEL_BREAK, '');
  if (/<\/?[A-Za-z][^>\r\n]*>/.test(neutral)) return rejected('html');

  if (STYLE_URL.test(body)) return rejected('external-resource');
  if (UNSAFE_STYLE.test(body)) return rejected('unsafe-style');

  return { ok: true, family, source };
}

// 解析后的图模型检查,绘制前的最后一道。绘制在当前页面里进行,模型里只要有带地址的图片或样式
// 里的外部引用,浏览器会当场发请求,等画完再查 SVG 就晚了。源码层的检查认的是写法,写法可以换;
// 这里认的是 Mermaid 自己解析出来的结果,与写法无关:
// - 任何 `img` 字段有值(流程图的图片形状);
// - 任何 `properties` 里带 `icon`(时序图参与者图标);
// - 名字带 style 的字段下面出现 url()、@import、expression() 这类写法(各种样式语句)。
// 标签文字里的 `getUrl()` 不在样式字段下面,不受影响。返回原因,没问题返回空串。
const MODEL_UNSAFE_STYLE = /url\s*\(|expression\s*\(|-moz-binding|behavior\s*:|@(?:import|font-face|namespace|supports|media|document|page)\b/i;
const MODEL_NODE_LIMIT = 200000;

export function inspectMermaidModel(model) {
  const seen = new WeakSet();
  const stack = [[model, false]];
  let visited = 0;
  while (stack.length) {
    const [value, inStyle] = stack.pop();
    if (typeof value === 'string') {
      if (inStyle && MODEL_UNSAFE_STYLE.test(value)) return 'external-resource';
      continue;
    }
    if (!value || typeof value !== 'object' || seen.has(value)) continue;
    seen.add(value);
    visited += 1;
    if (visited > MODEL_NODE_LIMIT) return 'too-large';
    if (value instanceof Map || value instanceof Set) {
      for (const entry of value.values()) stack.push([entry, inStyle]);
      continue;
    }
    if (Array.isArray(value)) {
      for (const entry of value) stack.push([entry, inStyle]);
      continue;
    }
    let keys;
    try {
      keys = Object.keys(value);
    } catch {
      continue;
    }
    for (const key of keys) {
      let child;
      try {
        child = value[key];
      } catch {
        continue;
      }
      if (key === 'img' && typeof child === 'string' && child.trim()) return 'external-resource';
      if (key === 'properties' && child && typeof child === 'object' && child.icon) return 'external-resource';
      stack.push([child, inStyle || /style/i.test(key)]);
    }
  }
  return '';
}

// Mermaid 报的行号是它自己预处理之后的行号:注释行连同它前面的空行整段删掉,开头的空行去掉,
// 流程图里 `}` 结尾的行后面的空行也并掉(mermaid 11 的 cleanupComments 与流程图解析入口)。
// 这里按同样的规则数一遍,把行号对回交给它的文本(与原文行数一致)。超出范围返回 0,不显示行号。
export function mermaidErrorLine(renderSource, reportedLine, family = '') {
  const reported = Number(reportedLine);
  if (!Number.isInteger(reported) || reported < 1) return 0;
  const lines = String(renderSource || '').split('\n');
  const kept = [];
  let blanks = [];
  let afterBrace = false;
  lines.forEach((line, index) => {
    if (!line.trim()) {
      if (!afterBrace) blanks.push(index);
      return;
    }
    if (/^\s*%%(?!\{)[^\n]+/.test(line)) {
      blanks = [];
      return;
    }
    if (kept.length) kept.push(...blanks);
    blanks = [];
    kept.push(index);
    afterBrace = family === 'flowchart' && line.trimEnd().endsWith('}');
  });
  return reported <= kept.length ? kept[reported - 1] + 1 : 0;
}
