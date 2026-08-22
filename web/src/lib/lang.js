// 文件扩展名 → highlight.js 语言名映射。SidePanel 预览与新建文件源码框
// 共用这张表；任何未识别扩展名都返回 ''，只 escape、不自动猜测语言。

const EXT_TO_LANG = {
  c: 'c',
  h: 'c',
  cpp: 'cpp',
  cxx: 'cpp',
  cc: 'cpp',
  hpp: 'cpp',
  hxx: 'cpp',
  inl: 'cpp',
  cs: 'csharp',
  java: 'java',
  kt: 'kotlin',
  kts: 'kotlin',
  js: 'javascript',
  jsx: 'javascript',
  mjs: 'javascript',
  cjs: 'javascript',
  ts: 'typescript',
  tsx: 'typescript',
  py: 'python',
  pyi: 'python',
  sh: 'bash',
  bash: 'bash',
  zsh: 'bash',
  ps1: 'powershell',
  psm1: 'powershell',
  psd1: 'powershell',
  json: 'json',
  jsonc: 'json',
  html: 'xml',
  htm: 'xml',
  xhtml: 'xml',
  xml: 'xml',
  svg: 'xml',
  vue: 'xml',
  svelte: 'xml',
  css: 'css',
  scss: 'scss',
  less: 'less',
  diff: 'diff',
  patch: 'diff',
  md: 'markdown',
  markdown: 'markdown',
  rs: 'rust',
  go: 'go',
  rb: 'ruby',
  php: 'php',
  sql: 'sql',
  swift: 'swift',
  lua: 'lua',
  pl: 'perl',
  pm: 'perl',
  r: 'r',
  m: 'objectivec',
  mm: 'objectivec',
  vb: 'vbnet',
  graphql: 'graphql',
  gql: 'graphql',
  ini: 'ini',
  toml: 'ini',
  mk: 'makefile',
  wat: 'wasm',
  cmake: 'cmake',
  yaml: 'yaml',
  yml: 'yaml',
};

// 常见无后缀或特殊文件名。
const NAME_TO_LANG = {
  Dockerfile: 'dockerfile',
  Makefile: 'makefile',
  'CMakeLists.txt': 'cmake',
  Gemfile: 'ruby',
  Rakefile: 'ruby',
  '.gitignore': '',
  '.npmrc': '',
};

/**
 * @param {string} pathOrName 完整路径或仅文件名
 * @returns {string} 注册过的 hljs language id,或 '' 表示不上色
 */
export function langForFile(pathOrName) {
  if (!pathOrName) return '';
  const name = String(pathOrName).split(/[\\/]/).pop() || '';
  if (NAME_TO_LANG[name] !== undefined) return NAME_TO_LANG[name];
  // 取最后一个 . 之后的后缀,转小写
  const dot = name.lastIndexOf('.');
  if (dot < 0 || dot === name.length - 1) return '';
  const ext = name.slice(dot + 1).toLowerCase();
  return EXT_TO_LANG[ext] || '';
}
