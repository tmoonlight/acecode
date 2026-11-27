import assert from 'node:assert/strict';
import { skillFilePathFromCatalog, skillViewDocument } from './skillViewDocument.js';
import { presentTool } from './toolPresentation.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const t = (key, options) => options?.defaultValue ?? key;

// 触发场景:模型调 skill_view 读取 SKILL.md,daemon 返回 {success, name, description, content, linked_files}。
// 期望行为:展开时拿到的是 markdown 正文本身。
// 回归:之前展开只看到整段 JSON,正文挤在 "content" 字符串里,换行全是 \n。
run('读取 SKILL.md 的结果取出 markdown 正文', () => {
  const output = JSON.stringify({
    success: true,
    name: 'review',
    description: 'Review source',
    content: '# Review\r\n\r\n- 先读改动\r\n- 再写结论\r\n',
    linked_files: ['references/checklist.md'],
  });
  assert.deepEqual(skillViewDocument('skill_view', output), {
    kind: 'markdown',
    name: 'review',
    path: 'SKILL.md',
    content: '# Review\n\n- 先读改动\n- 再写结论\n',
  });
});

// 触发场景:daemon 读不到去掉 frontmatter 的正文时退回整份文件,开头带 --- 元数据段。
// 期望行为:元数据段剥掉,只渲染正文;否则 `description: ...` 加下一行 --- 会被当成二级标题。
run('开头的 frontmatter 不进渲染', () => {
  const output = JSON.stringify({
    success: true,
    name: 'review',
    content: '---\nname: review\ndescription: Review source\n---\n\n# Review\n正文\n',
  });
  assert.equal(skillViewDocument('skill_view', output).content, '\n# Review\n正文\n');
});

// 触发场景:正文中间出现 ---(markdown 分隔线),开头不是 frontmatter。
// 期望行为:原样保留,不能误删正文。
run('正文中间的分隔线不当 frontmatter', () => {
  const content = '# Title\n\n---\n\nmore\n---\n';
  const output = JSON.stringify({ success: true, name: 'x', content });
  assert.equal(skillViewDocument('skill_view', output).content, content);
});

// 触发场景:附属 markdown 以分隔线开头,后面某处又有一条 ---。
// 期望行为:第一行不是 `key:` 就不算元数据段,两条线之间的正文原样保留。
run('以分隔线开头的正文不被误删', () => {
  const content = '---\n\n## 用法\n\n说明文字\n\n---\n\n结尾\n';
  const output = JSON.stringify({ success: true, name: 'x', file_path: 'references/usage.md', content });
  assert.equal(skillViewDocument('skill_view', output).content, content);
});

// 触发场景:带 file_path 读取附属文件。
// 期望行为:.md 按 markdown 渲染;脚本按源码显示并带上路径,用于选择高亮语言。
run('附属文件按扩展名区分 markdown 与源码', () => {
  const md = skillViewDocument('skill_view', JSON.stringify({
    success: true, name: 'review', file_path: 'references/api.MD', content: '## API\n',
  }));
  assert.equal(md.kind, 'markdown');
  assert.equal(md.path, 'references/api.MD');

  const script = skillViewDocument('skill_view', JSON.stringify({
    success: true, name: 'review', file_path: 'scripts/run.py', content: 'print("ok")\r\n',
  }));
  assert.deepEqual(script, { kind: 'source', name: 'review', path: 'scripts/run.py', content: 'print("ok")\n' });
});

// 触发场景:失败结果、被截断的 JSON、旧会话压缩后的占位文字、空正文、别的工具。
// 期望行为:一律返回 null,调用方照旧显示原文,不能画出空框。
run('认不出来的结果返回 null', () => {
  const cases = [
    ['skill_view', JSON.stringify({ success: false, error: "Skill 'x' not found." })],
    ['skill_view', '{"success":true,"name":"x","content":"# 被截'],
    ['skill_view', '[Old tool result content cleared]'],
    ['skill_view', JSON.stringify({ success: true, name: 'x', content: '   \n' })],
    ['skill_view', JSON.stringify({ success: true, name: 'x', content: '---\nname: x\n---\n' })],
    ['skill_view', JSON.stringify({ success: true, name: 'x', content: 42 })],
    ['skill_view', JSON.stringify([{ success: true, content: '# x' }])],
    ['skill_view', ''],
    ['skill_view', undefined],
    ['file_read', JSON.stringify({ success: true, content: '# x' })],
  ];
  for (const [tool, output] of cases) {
    assert.equal(skillViewDocument(tool, output), null, `${tool}: ${String(output)}`);
  }
});

// 触发场景:恢复历史会话时工具消息不带工具名,只有 summary.verb = skill_view;
// 旧格式的调用把「工具调用 / 工具返回」拼进 output,结果单独放在 resultOutput。
// 期望行为:ToolBlock 传入的 presentation.tool / presentation.output 都能认出文档。
run('历史会话与旧格式调用经 presentTool 也能认出', () => {
  const result = JSON.stringify({ success: true, name: 'review', content: '# Review\n' });
  const history = presentTool({ isDone: true, summary: { verb: 'skill_view', object: 'review loaded' }, output: result }, t);
  assert.equal(skillViewDocument(history.tool, history.output)?.content, '# Review\n');

  const legacy = presentTool({
    isDone: true,
    tool: 'skill_view',
    output: `工具调用\n{"name":"review"}\n\n工具返回\n${result}`,
    resultOutput: result,
  }, t);
  assert.equal(skillViewDocument(legacy.tool, legacy.output)?.kind, 'markdown');
});

// 触发场景:技能预览框右上角点「打开文件夹」,前端拿 /api/skills 的清单按技能名找 SKILL.md。
// 期望行为:按 name 精确匹配取 path(资源管理器会打开技能目录并选中 SKILL.md);
// 技能已被删除、改名或清单不是数组时返回空串,由调用方提示「找不到」,不能静默。
run('按技能名从清单里取 SKILL.md 路径', () => {
  const skills = [
    { name: 'review-helper', path: 'C:/Users/u/.acecode/skills/review-helper/SKILL.md' },
    { name: 'review', path: ' C:/Users/u/.acecode/skills/review/SKILL.md ' },
    null,
    { name: 'no-path' },
  ];
  assert.equal(skillFilePathFromCatalog(skills, 'review'), 'C:/Users/u/.acecode/skills/review/SKILL.md');
  assert.equal(skillFilePathFromCatalog(skills, ' review-helper '), 'C:/Users/u/.acecode/skills/review-helper/SKILL.md');
  assert.equal(skillFilePathFromCatalog(skills, 'Review'), '');
  assert.equal(skillFilePathFromCatalog(skills, 'missing'), '');
  assert.equal(skillFilePathFromCatalog(skills, 'no-path'), '');
  assert.equal(skillFilePathFromCatalog(skills, ''), '');
  assert.equal(skillFilePathFromCatalog({ error: 'workspace not found' }, 'review'), '');
  assert.equal(skillFilePathFromCatalog(undefined, 'review'), '');
});
