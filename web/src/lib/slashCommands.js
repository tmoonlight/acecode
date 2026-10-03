// SlashDropdown 的纯逻辑层:命令排序 + 输入框首段命令解析。
//
// 排序规则(design.md D4):
//   1. name 前缀匹配优先(case-insensitive)
//   2. name 子串匹配次之
//   3. description 子串匹配最后
//   4. 同档内按 name 字典序
// 完全不命中 → 过滤掉。空查询返回原顺序(builtin 在前 + skill 字典序),不打分。
//
// 别名:一条命令可以有多个别名(item.aliases),别名不单独占一行。别名按 name
// 同样的前缀 / 子串档位参与匹配,命令取最好的一档;只有别名比原名匹配得更好时,
// 下拉才显示成 "原名 (别名)"(slashCommandMatchedAlias),选中后插入原名。

import { tr } from '../i18n/index.js';

function lower(s) {
  return typeof s === 'string' ? s.toLowerCase() : '';
}

// 基础 builtin 命令表(也是下拉空查询时的顺序)。别名与 TUI 注册保持一致:
// /btw 的别名 side,/remote-control 的别名 rc。
const BUILTIN_COMMANDS = Object.freeze([
  { name: 'init', descriptionKey: 'init' },
  { name: 'compact', descriptionKey: 'compact' },
  { name: 'feedback', descriptionKey: 'feedback' },
  { name: 'goal', descriptionKey: 'goal' },
  { name: 'plan', descriptionKey: 'plan' },
  { name: 'turn', descriptionKey: 'turn' },
  { name: 'btw', descriptionKey: 'btw', aliases: Object.freeze(['side']) },
  { name: 'lsp', descriptionKey: 'lsp' },
  { name: 'sandbox', descriptionKey: 'sandbox' },
  { name: 'memory', descriptionKey: 'memory' },
  { name: 'remote-control', descriptionKey: 'remoteControl', aliases: Object.freeze(['rc']) },
]);

const BUILTIN_BY_NAME = new Map(BUILTIN_COMMANDS.map((c) => [c.name, c]));
const BUILTIN_BY_ALIAS = new Map(
  BUILTIN_COMMANDS.flatMap((c) => (c.aliases || []).map((alias) => [alias, c])),
);

function builtinDefinition(name) {
  return BUILTIN_BY_NAME.get(name) || BUILTIN_BY_ALIAS.get(name) || null;
}

// 命令的别名数组(去空、去重、去掉与原名相同的项);没有别名返回空数组。
export function commandAliases(item) {
  const name = item && item.name;
  const out = [];
  for (const alias of Array.isArray(item?.aliases) ? item.aliases : []) {
    if (typeof alias !== 'string' || !alias || alias === name || out.includes(alias)) continue;
    out.push(alias);
  }
  return out;
}

function withAliases(item, aliases) {
  const list = commandAliases({ name: item.name, aliases });
  return list.length > 0 ? { ...item, aliases: list } : item;
}

export function builtinCommandDescription(name, fallback = '') {
  const def = builtinDefinition(name);
  return def ? tr(`commands.descriptions.${def.descriptionKey}`) : fallback;
}

export function fallbackCommands() {
  return BUILTIN_COMMANDS.map((c) => withAliases({
    kind: 'builtin',
    name: c.name,
    description: builtinCommandDescription(c.name),
  }, c.aliases));
}

// 把后端返回的 {builtins, commands, skills} 拍平成统一项数组,加 kind 字段以备扩展。
// 顺序:builtins 先(后端固定 init→compact→goal→plan),opencode command 后,skills 后。
export function flattenCommands(payload) {
  const out = [];
  if (payload && Array.isArray(payload.builtins)) {
    for (const c of payload.builtins) {
      if (!c || !c.name) continue;
      // 旧 daemon 把别名(如 rc)单列成一条 builtin;它已经挂在原名的 aliases 上,丢掉。
      if (BUILTIN_BY_ALIAS.has(c.name)) continue;
      const def = BUILTIN_BY_NAME.get(c.name);
      out.push(withAliases({
        kind: 'builtin',
        name: c.name,
        description: builtinCommandDescription(c.name, c.description || ''),
      }, [...(Array.isArray(c.aliases) ? c.aliases : []), ...(def?.aliases || [])]));
    }
  }
  if (payload && Array.isArray(payload.commands)) {
    for (const c of payload.commands) {
      if (c && c.name) out.push({ kind: 'command', name: c.name, description: c.description || '' });
    }
  }
  if (payload && Array.isArray(payload.skills)) {
    for (const c of payload.skills) {
      if (c && c.name) out.push({
        kind: 'skill', name: c.name, description: c.description || '',
        ...(c.path ? { path: c.path } : {}),
        ...(c.mention ? { mention: c.mention } : {}),
      });
    }
  }
  return out;
}

export function commandsWithFallback(payload) {
  const commands = flattenCommands(payload);
  const fallbackBuiltins = fallbackCommands();
  // 后端返回的 builtin 覆盖同名基础命令(描述以本地 i18n 为准,别名已在 flatten 时合并)。
  const fallbackNames = new Set(fallbackBuiltins.map((c) => c.name));
  const payloadBuiltins = new Map(
    commands
      .filter((c) => c.kind === 'builtin' && fallbackNames.has(c.name))
      .map((c) => [c.name, c]),
  );
  const mergedBuiltins = fallbackBuiltins.map((c) => payloadBuiltins.get(c.name) || c);
  const rest = commands.filter((c) => c.kind !== 'builtin' || !fallbackNames.has(c.name));
  return [...mergedBuiltins, ...rest];
}

function nameScore(name, query) {
  const value = lower(name);
  if (value.startsWith(query)) return 1000;
  if (value.includes(query)) return 500;
  return 0;
}

// 只看原名与描述的分数。query 已 lowercase。
function scoreCommandName(item, query) {
  if (!query) return 1; // 空查询人人有份,排序回退到原顺序
  const score = nameScore(item.name, query);
  if (score > 0) return score;
  if (lower(item.description).includes(query)) return 100;
  return 0;
}

// 原名、描述、别名里最好的一档,以及(别名胜出时)胜出的别名。
function matchCommand(item, query) {
  let score = scoreCommandName(item, query);
  let alias = '';
  if (query) {
    for (const candidate of commandAliases(item)) {
      const aliasScore = nameScore(candidate, query);
      if (aliasScore > score) {
        score = aliasScore;
        alias = candidate;
      }
    }
  }
  return { score, alias };
}

// 下拉行要标出的别名:查询命中别名、且别名比原名(含描述)匹配得更好时返回该别名,
// 否则返回空串。敲 "rc" → "remote-control (rc)";敲 "re" 或空查询 → 只显示原名。
export function slashCommandMatchedAlias(item, query) {
  const q = lower(query || '').trim();
  if (!item || !q) return '';
  return matchCommand(item, q).alias;
}

// 排序:分数降序,同分按 name 字典序。返回新数组,不修改输入。
export function rankCommands(query, items) {
  const q = lower(query || '').trim();
  const scored = [];
  for (const it of items || []) {
    const s = matchCommand(it, q).score;
    if (s > 0) scored.push({ it, s });
  }
  if (!q) {
    // 空查询保留 flattenCommands 的原始顺序(builtin 在前)
    return scored.map((x) => x.it);
  }
  scored.sort((a, b) => {
    if (b.s !== a.s) return b.s - a.s;
    return a.it.name.localeCompare(b.it.name);
  });
  return scored.map((x) => x.it);
}

export function slashCommandKindPresentation(item) {
  if (item && item.kind === 'builtin') {
    return { icon: lower(item.name) === 'goal' ? 'Goal' : 'tool', label: tr('commands.kindBuiltin') };
  }
  if (item && item.kind === 'command') {
    return { icon: 'command', label: tr('commands.kindCommand') };
  }
  return { icon: 'lightbulb', label: tr('commands.kindSkill') };
}

// 解析输入框值的首段命令名:从首字符 `/` 到第一个空白(或字符串末尾)之间的串,
// 去掉前导 `/`。返回 {name, headLength}:
//   - name: 已知命令名(在 knownNames 中)→ 该名;否则 null
//   - headLength: 首段(含 `/`)在原串中的字符长度,无论是否命中
// 空白判定:空格 / \t / \n / \r。
export function parseLeadingCommand(value, knownNames = []) {
  if (typeof value !== 'string' || value.length === 0 || value[0] !== '/') {
    return { name: null, headLength: 0 };
  }
  let end = 1;
  while (end < value.length && !/\s/.test(value[end])) end++;
  const head = value.slice(1, end);
  const known = new Set(knownNames || []);
  return {
    name: known.has(head) ? head : null,
    headLength: end,
  };
}

export function leadingCommandBlockEnd(value, leading) {
  if (typeof value !== 'string' || !leading?.name || !Number.isFinite(leading.headLength)) {
    return 0;
  }
  let end = Math.max(0, Math.min(value.length, leading.headLength));
  if (end < value.length && /\s/.test(value[end])) end += 1;
  return end;
}

export function deleteLeadingCommandBlock(value, leading, selectionStart, selectionEnd, direction = 'backward') {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));
  const hasSelection = start !== end;

  const touchesBlock = hasSelection
    ? start < blockEnd && end > 0
    : direction === 'forward'
      ? start < blockEnd
      : start > 0 && start <= blockEnd;
  if (!touchesBlock) return null;

  let deleteEnd = hasSelection ? Math.max(blockEnd, end) : blockEnd;
  if (deleteEnd < value.length && /\s/.test(value[deleteEnd])) deleteEnd += 1;
  return {
    value: value.slice(deleteEnd),
    selectionStart: 0,
    selectionEnd: 0,
  };
}

export function normalizeLeadingCommandSelection(value, leading, selectionStart, selectionEnd) {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));

  if (start === end) {
    if (start > 0 && start < blockEnd) {
      return { selectionStart: blockEnd, selectionEnd: blockEnd };
    }
    return null;
  }

  let nextStart = start;
  let nextEnd = end;
  if (nextStart > 0 && nextStart < blockEnd) nextStart = 0;
  if (nextEnd > 0 && nextEnd < blockEnd) nextEnd = blockEnd;

  if (nextStart === start && nextEnd === end) return null;
  return { selectionStart: nextStart, selectionEnd: nextEnd };
}

export function moveAcrossLeadingCommandBlock(value, leading, selectionStart, selectionEnd, direction) {
  if (typeof value !== 'string' || !leading?.name) return null;
  const blockEnd = leadingCommandBlockEnd(value, leading);
  if (blockEnd <= 0) return null;

  const rawStart = Number.isFinite(selectionStart) ? selectionStart : 0;
  const rawEnd = Number.isFinite(selectionEnd) ? selectionEnd : rawStart;
  const start = Math.max(0, Math.min(value.length, Math.min(rawStart, rawEnd)));
  const end = Math.max(start, Math.min(value.length, Math.max(rawStart, rawEnd)));
  if (start !== end) return null;

  if (direction === 'backward' && start > 0 && start <= blockEnd) {
    return { selectionStart: 0, selectionEnd: 0 };
  }
  if (direction === 'forward' && start >= 0 && start < blockEnd) {
    return { selectionStart: blockEnd, selectionEnd: blockEnd };
  }
  return null;
}

// 解析一条已落库消息的首段斜杠命令,供 transcript 渲染成徽标(chip)使用。
// 与 parseLeadingCommand 的差别:这里额外把命中的命令在 commands 清单里找回
// kind / description,并把首段(含 `/`)与剩余正文切开,方便 UI 分段渲染。
//
// 返回:
//   - 命中已知命令 → { token, name, kind, description, rest }
//       token   = 首段含前导 `/`(如 "/taobao-compare")
//       rest    = 首段之后的全部原文(含分隔空白,交给 whitespace-pre-wrap 保留)
//   - 不以 `/` 开头 / 首段不是已知命令(未命中 skill)→ null
//     调用方据此回退到纯文本渲染,避免把 `/foobar` 误当命令高亮。
export function resolveLeadingSlashCommand(text, commands = []) {
  if (typeof text !== 'string' || text.length === 0 || text[0] !== '/') return null;
  const list = Array.isArray(commands) ? commands.filter(Boolean) : [];
  // 别名同样识别成命令徽标(如 "/rc show"),name 保留用户实际敲的名字。
  const knownNames = list.flatMap((c) => [c.name, ...commandAliases(c)]).filter(Boolean);
  const leading = parseLeadingCommand(text, knownNames);
  if (!leading.name) return null;
  const item = list.find((c) => c.name === leading.name)
    || list.find((c) => commandAliases(c).includes(leading.name))
    || null;
  return {
    token: text.slice(0, leading.headLength),
    name: leading.name,
    kind: item && item.kind ? item.kind : 'skill',
    description: item && item.description ? item.description : '',
    rest: text.slice(leading.headLength),
  };
}

export function parseExecutableBuiltinCommand(value) {
  const text = typeof value === 'string' ? value.trim() : '';
  const leading = parseLeadingCommand(text, ['init', 'compact', 'goal', 'plan', 'lsp', 'sandbox', 'memory', 'rc', 'remote-control']);
  if (!leading.name) return null;
  return {
    name: leading.name,
    args: text.slice(leading.headLength).trim(),
    display_text: text,
  };
}

// Slash commands remain leading-only; skills can be selected at any caret.
export function commandQueryAtCursor(value = '', cursor = String(value).length) {
  const text = String(value);
  const caret = Math.max(0, Math.min(text.length, Number(cursor) || 0));
  // Skill names use identifier characters. Prose in Chinese and punctuation
  // can touch the trigger without being consumed as part of its replacement.
  let begin = caret - 1;
  while (begin >= 0 && /[A-Za-z0-9_.:-]/.test(text[begin])) begin -= 1;
  const trigger = text[begin];
  if (trigger !== '/' && trigger !== '$') return null;
  if (begin > 0 && /[A-Za-z0-9_.:/\\$-]/.test(text[begin - 1])) return null;
  let end = caret;
  while (end < text.length && /[A-Za-z0-9_.:-]/.test(text[end])) end += 1;
  return { begin, end, query: text.slice(begin + 1, caret), leading: begin === 0 && trigger === '/', trigger };
}
