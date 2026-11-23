// 模型常见的 Mermaid 语法小错,在官方解析器报错之后才修一遍再试(纯字符串逻辑)。
//
// 只在解析失败后调用:合法的图不经过这里,交给 Mermaid 的文本一个字都不改。修完由调用方重新
// 解析,还不行就照旧显示源码,所以这里宁可少修,不去猜。修的内容只有:给标签补引号、把引号换成
// 实体码、给撞了关键字的节点改名、补齐或去掉 `end`、去掉对不上的激活标记、给带特殊字符的名字
// 起别名。不会产生新的语句。
//
// 输入是已经过 prepareMermaidRenderSource 的文本(尖括号、实体都改写过)。

const WORD = '\\w\\u0080-\\uFFFF';

function quoteLabel(text) {
  let inner = String(text);
  const trimmed = inner.trim();
  if (trimmed.length >= 2 && trimmed.startsWith('"') && trimmed.endsWith('"')) {
    inner = trimmed.slice(1, -1);
  }
  inner = inner.replace(/"/g, '#quot;');
  return `"${inner.trim() ? inner : ' '}"`;
}

// ---- 流程图 ----

// 节点形状的开合符号,长的在前。`[/` 开头的四种是平行四边形 / 梯形。
const FLOW_SHAPES = [
  ['(((', ')))', '()'], ['((', '))', '()'], ['([', '])', '[]'], ['[[', ']]', '[]'],
  ['[(', ')]', '()'], ['{{', '}}', '{}'],
  ['[/', '/]', ''], ['[/', '\\]', ''], ['[\\', '\\]', ''], ['[\\', '/]', ''],
  ['[', ']', ''], ['(', ')', ''], ['{', '}', ''], ['>', ']', ''],
];
const FLOW_ID = new RegExp(`^[${WORD}]+(?:[.\\-/](?![-.>=])[${WORD}]+)*`);
const FLOW_LINK = /^(?:<|[ox](?=-{2}|={2}|-\.))?(?:-\.+->?|-{2,}(?:[>ox](?![\w\u0080-￿])|>)?|={2,}(?:[>ox](?![\w\u0080-￿])|>)?|~{3,})/;
// `A -- 文字 --> B`、`A -. 文字 .-> B`、`A == 文字 ==> B`:文字里有括号之类就解析不了,换成
// `A -->|"文字"| B` 的写法。
const FLOW_INLINE_LINK = /^(--|==|-\.)[ \t]+(\S.*?)[ \t]+(-{2,}[>ox]?|={2,}[>ox]?|\.-+[>ox]?)(?=[\s"]|[\w\u0080-￿]|$)/;
// 节点后面还能跟什么:行尾、分号、注释、样式类、`&`、连线。闭合符号后面不是这些,就说明标签
// 文字里自己带了同样的符号,继续往后找。
const FLOW_AFTER_NODE = /^\s*(?:$|;|%%|:::|&|(?:<|[ox](?=-{2}|={2}|-\.))?(?:-{2,}|={2,}|-\.|~{3,}))/;
// 当节点名用会被解析成关键字的词。改名并把原名留作显示文字。
const FLOW_RESERVED = new Set([
  'end', 'default', 'style', 'class', 'classDef', 'linkStyle', 'click', 'graph', 'flowchart',
  'subgraph', 'direction', 'call', 'href',
]);
const FLOW_CLASS_SUFFIX = new RegExp(`^:::[${WORD}-]*`);
const FLOW_STATEMENT = /^[ \t]*(?:style|classDef|class|cssClass|linkStyle|click|direction|accTitle|accDescr|graph|flowchart|flowchart-elk)\b(?![ \t]*(?:[[({]|:::|&|-{2}|={2}|-\.|~{3}))/i;

function balanced(text, pair) {
  if (!pair) return true;
  let depth = 0;
  for (const ch of text) {
    if (ch === pair[0]) depth += 1;
    else if (ch === pair[1]) depth -= 1;
    if (depth < 0) return false;
  }
  return depth === 0;
}

// 从 start(紧跟在节点名后面)认出一个形状,返回 { open, close, text, end };认不出返回 null。
// 几种形状都对得上时取收得最早的那个(`[/api/users]` 是方框,不是一路找到下一个 `/]` 的平行
// 四边形),一样早就取符号长的(`((圆))` 是圆形,不是标签为 `(圆)` 的圆角框)。
function matchFlowShape(line, start) {
  let best = null;
  for (const [open, close, pair] of FLOW_SHAPES) {
    if (!line.startsWith(open, start)) continue;
    let from = start + open.length;
    for (;;) {
      const at = line.indexOf(close, from);
      if (at < 0) break;
      if (FLOW_AFTER_NODE.test(line.slice(at + close.length))) {
        const text = line.slice(start + open.length, at);
        const end = at + close.length;
        if (balanced(text, pair) && (!best || end < best.end)) best = { open, close, text, end };
        break;
      }
      from = at + 1;
    }
  }
  return best;
}

// labeled:整张图里带过形状(也就是有自己的显示文字)的关键字节点名。Mermaid 里后写的标签会盖掉
// 先写的,所以别处已有标签的,这里的裸引用只改名、不再补显示文字。
function repairFlowStatement(line, labeled) {
  let out = '';
  let index = 0;
  let statementStart = true;
  while (index < line.length) {
    const rest = line.slice(index);
    const space = /^\s+/.exec(rest);
    if (space) {
      out += space[0];
      index += space[0].length;
      continue;
    }
    // Mermaid 只认整行注释,写在语句后面的 `%% ...` 会让整行解析失败。
    if (rest.startsWith('%%') && /(?:^|\s)$/.test(out)) return out.trimEnd();
    if (rest[0] === ';') {
      out += ';';
      index += 1;
      statementStart = true;
      continue;
    }
    if (statementStart && FLOW_STATEMENT.test(rest)) {
      const stop = rest.indexOf(';');
      const statement = stop < 0 ? rest : rest.slice(0, stop);
      out += statement;
      index += statement.length;
      continue;
    }
    statementStart = false;

    const classSuffix = FLOW_CLASS_SUFFIX.exec(rest);
    if (classSuffix) {
      out += classSuffix[0];
      index += classSuffix[0].length;
      continue;
    }
    const inline = FLOW_INLINE_LINK.exec(rest);
    if (inline) {
      const [, opener, text, closer] = inline;
      const arrow = opener === '-.' ? `-${closer}` : closer;
      out += `${arrow}|${quoteLabel(text)}|`;
      index += inline[0].length;
      continue;
    }
    const link = FLOW_LINK.exec(rest);
    if (link) {
      out += link[0];
      index += link[0].length;
      const label = /^(\s*)\|([^|]*)\|/.exec(line.slice(index));
      if (label) {
        out += `${label[1]}|${quoteLabel(label[2])}|`;
        index += label[0].length;
      }
      continue;
    }
    const id = FLOW_ID.exec(rest);
    if (id) {
      const reserved = FLOW_RESERVED.has(id[0]);
      const name = reserved ? `${id[0]}_` : id[0];
      index += id[0].length;
      const shape = matchFlowShape(line, index);
      if (shape) {
        if (reserved) labeled.add(id[0]);
        out += `${name}${shape.open}${quoteLabel(shape.text)}${shape.close}`;
        index = shape.end;
      } else {
        out += reserved && !labeled.has(id[0]) ? `${name}["${id[0]}"]` : name;
      }
      continue;
    }
    out += rest[0];
    index += 1;
  }
  return out;
}

// `subgraph 客户端 (Client)` 这类标题带空格或符号的,换成「内部名["标题"]」。
function repairFlowSubgraph(line, counter) {
  const match = /^([ \t]*subgraph)[ \t]+(.*?)[ \t]*$/i.exec(line);
  if (!match) return line;
  const [, head, title] = match;
  if (!title || /^".*"$/.test(title) || new RegExp(`^[${WORD}.-]+$`).test(title)) return line;
  const named = new RegExp(`^([${WORD}.-]+)[ \\t]*\\[(.*)\\]$`).exec(title);
  if (named) return `${head} ${named[1]}[${quoteLabel(named[2])}]`;
  return `${head} ace_sg_${counter.next()}[${quoteLabel(title)}]`;
}

function repairFlowchart(source) {
  const labeled = new Set();
  const pass = () => {
    let subgraphs = 0;
    const counter = { next: () => { subgraphs += 1; return subgraphs; } };
    let headerSeen = false;
    return source.split('\n').map((line) => {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith('%%')) return line;
      if (!headerSeen) {
        headerSeen = true;
        return line;
      }
      // 单独一行的 end 是子图结束;出现在别处的 end 才是被当成节点名用。
      if (/^end[ \t]*;?$/i.test(trimmed)) return line;
      if (/^subgraph\b/i.test(trimmed)) return repairFlowSubgraph(line, counter);
      return repairFlowStatement(line, labeled);
    }).join('\n');
  };
  // 走两遍:第一遍只为了收集哪些关键字节点名在图里带过标签(标签可能写在引用它的那一行后面)。
  pass();
  return pass();
}

// ---- 时序图 ----

const SEQUENCE_BLOCK_OPEN = /^[ \t]*(?:loop|alt|opt|par|par_over|critical|break|rect|box)\b/i;
const SEQUENCE_BLOCK_BRANCH = /^[ \t]*(?:else|and|option)\b/i;
const SEQUENCE_BLOCK_END = /^[ \t]*end[ \t]*$/i;

// 分组没收尾就在末尾补 `end`;多出来的 `end`、落在分组外面的 else / and / option 去掉。
function balanceSequenceBlocks(source) {
  let depth = 0;
  const lines = source.split('\n').map((line) => {
    if (SEQUENCE_BLOCK_OPEN.test(line)) {
      depth += 1;
      return line;
    }
    if (SEQUENCE_BLOCK_END.test(line)) {
      if (depth === 0) return '';
      depth -= 1;
      return line;
    }
    if (depth === 0 && SEQUENCE_BLOCK_BRANCH.test(line)) return '';
    return line;
  });
  for (; depth > 0; depth -= 1) lines.push('end');
  return lines.join('\n');
}

// 激活 / 取消激活对不上时 Mermaid 整张图拒画。去掉全部激活标记:少了生命线上的激活条,消息都在。
function stripSequenceActivations(source) {
  return source.split('\n').map((line) => {
    if (/^[ \t]*(?:de)?activate\b/i.test(line)) return '';
    const colon = line.indexOf(':');
    const head = colon < 0 ? line : line.slice(0, colon);
    const tail = colon < 0 ? '' : line.slice(colon);
    return head.replace(/(--?(?:>>|[>x)]))[ \t]*[+-]/g, '$1') + tail;
  }).join('\n');
}

function repairSequence(source, message) {
  const balancedSource = balanceSequenceBlocks(source);
  return /inactivate an inactive participant/i.test(message)
    ? stripSequenceActivations(balancedSource)
    : balancedSource;
}

// ---- 状态图 ----

// 带连字符的状态名(`pre-check`)解析不了:声明成 `state "pre-check" as pre_check`,再把连线、
// 描述、state 语句里用到它的地方换成别名。冒号后面的文字不动。
function repairState(source) {
  const lines = source.split('\n');
  const hyphenated = new RegExp(`(^|[^${WORD}"-])([${WORD}]+(?:-[${WORD}]+)+)(?![${WORD}"-])`, 'g');
  const aliases = new Map();
  const rewritten = lines.map((line) => {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('%%') || /^(?:stateDiagram|note|end note|classDef|class|style|direction)\b/i.test(trimmed)) {
      return line;
    }
    const colon = line.indexOf(':');
    const head = colon < 0 ? line : line.slice(0, colon);
    const tail = colon < 0 ? '' : line.slice(colon);
    if (head.includes('"')) return line;
    return head.replace(hyphenated, (match, lead, name) => {
      if (!aliases.has(name)) aliases.set(name, name.replace(/-/g, '_'));
      return lead + aliases.get(name);
    }) + tail;
  });
  if (!aliases.size) return source;
  const header = rewritten.findIndex((line) => /^\s*stateDiagram/.test(line));
  const declarations = Array.from(aliases, ([name, alias]) => `state "${name}" as ${alias}`);
  rewritten.splice(header + 1, 0, ...declarations);
  return rewritten.join('\n');
}

// ---- 类图 ----

// 成员区之外带 `::` 或连字符的类名(`ns::Type`、`my-class`)包上反引号;成员行里的花括号换成
// 实体码(成员区靠单独一行的 `}` 收尾,行内再出现花括号会被当成提前收尾)。
function repairClass(source) {
  const special = new RegExp(`[${WORD}]+(?:(?:::|-(?=[${WORD}]))[${WORD}]+)+`, 'g');
  const stack = [];
  return source.split('\n').map((line) => {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('%%') || /^classDiagram\b/.test(trimmed)) return line;
    const inMembers = stack[stack.length - 1] === 'class';
    if (inMembers) {
      if (trimmed === '}') {
        stack.pop();
        return line;
      }
      return line.replace(/\{/g, '#123;').replace(/\}/g, '#125;');
    }
    if (trimmed === '}') {
      stack.pop();
      return line;
    }
    const split = /[ \t]:(?!:)/.exec(line);
    const head = split ? line.slice(0, split.index) : line;
    const tail = split ? line.slice(split.index) : '';
    const fixed = head.includes('`') || head.includes('"')
      ? head
      : head.replace(special, (name, offset) => (
        /^(?:class|namespace|note|for|direction|style|cssClass|classDef)$/i.test(name) || head[offset - 1] === '~' ? name : `\`${name}\``
      ));
    if (/\{[ \t]*$/.test(fixed)) stack.push(/^[ \t]*namespace\b/i.test(fixed) ? 'namespace' : 'class');
    return fixed + tail;
  }).join('\n');
}

// ---- ER 图 ----

// 关系后面漏了 `: 说明`,补一个空说明。
const ER_RELATION_WITHOUT_LABEL = /^([ \t]*(?:"[^"\n]+"|[^\s:{}"]+)[ \t]+[|}o][|o{}]?(?:--|\.\.)[|o{}]?[|{o][ \t]+(?:"[^"\n]+"|[^\s:{}"]+))[ \t]*$/;

function repairEr(source) {
  return source.split('\n').map((line) => {
    const match = ER_RELATION_WITHOUT_LABEL.exec(line);
    return match ? `${match[1]} : ""` : line;
  }).join('\n');
}

// 返回修过的文本;没什么可修的时候返回原文本(调用方据此决定要不要再解析一次)。
export function repairMermaidSource(source, family, message = '') {
  const text = String(source || '');
  try {
    if (family === 'flowchart') return repairFlowchart(text);
    if (family === 'sequence') return repairSequence(text, String(message || ''));
    if (family === 'state') return repairState(text);
    if (family === 'class') return repairClass(text);
    if (family === 'er') return repairEr(text);
  } catch {
    // 修不了就当没修。
  }
  return text;
}
