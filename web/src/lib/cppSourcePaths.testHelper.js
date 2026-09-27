import { readFileSync } from 'node:fs';

const repoRoot = new URL('../../../', import.meta.url);
const mappings = JSON.parse(readFileSync(new URL('tests/cpp_source_paths.json', repoRoot), 'utf8'));

// C++ 目录搬迁时只更新这张表;路径失效必须让架构测试明确失败。
export function cppSourcePath(key) {
  const relativePath = mappings[key];
  if (typeof relativePath !== 'string' || !relativePath) {
    throw new Error(`Missing C++ source mapping: ${key}`);
  }
  return relativePath;
}

export function readCppSource(key) {
  const relativePath = cppSourcePath(key);
  try {
    return readFileSync(new URL(relativePath, repoRoot), 'utf8');
  } catch (cause) {
    throw new Error(`Cannot read C++ source mapping ${key}: ${relativePath}`, { cause });
  }
}
