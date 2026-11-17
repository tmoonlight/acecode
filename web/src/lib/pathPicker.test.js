import assert from 'node:assert/strict';
import {
  PICKER_MODE_FILE,
  PICKER_MODE_FOLDER,
  browseErrorKey,
  initialPickerPath,
  isPickerRoot,
  isSelectableEntry,
  joinPickerPath,
  mkdirErrorKey,
  NEW_FOLDER_NAME_MAX_LENGTH,
  newFolderNameProblem,
  normalizePickerPath,
  pickerBreadcrumbs,
  pickerNamesCaseInsensitive,
  pickerParentPath,
  pickerSelectionTarget,
  rememberedFolderAfterPick,
  rootDisplayName,
  rootUsagePercent,
  selectableEntries,
  sortPickerEntries,
  typeaheadMatch,
  validatePathPickerPrefs,
} from './pathPicker.js';

function test(name, fn) {
  try {
    fn();
    console.log(`  ✓ ${name}`);
  } catch (error) {
    console.error(`  ✗ ${name}`);
    throw error;
  }
}

// 场景:用户在路径框里粘贴各种写法的 Windows 路径。期望:统一成正斜杠 + 大写盘符,
// 盘根保留尾斜杠,普通目录不带尾斜杠,`..` 与 `.` 被词法折叠;相对路径与空串返回 ''。
test('normalizes Windows spellings without touching junction semantics', () => {
  assert.equal(normalizePickerPath('c:\\Users\\shao\\'), 'C:/Users/shao');
  assert.equal(normalizePickerPath('C:/Users/./shao/../shao'), 'C:/Users/shao');
  assert.equal(normalizePickerPath('c:\\'), 'C:/');
  assert.equal(normalizePickerPath('C:'), 'C:/');
  assert.equal(normalizePickerPath('C:\\Users\\..\\..'), 'C:/');
  assert.equal(normalizePickerPath('\\\\server\\share\\dir'), '//server/share/dir');
  assert.equal(normalizePickerPath('relative\\dir'), '');
  assert.equal(normalizePickerPath('   '), '');
  assert.equal(normalizePickerPath(null), '');
});

// 场景:POSIX 路径。期望:根保留 "/",多余斜杠折叠,`..` 不会越过根。
test('normalizes POSIX spellings', () => {
  assert.equal(normalizePickerPath('/home/me/'), '/home/me');
  assert.equal(normalizePickerPath('//home///me'), '//home/me');
  assert.equal(normalizePickerPath('/home/../..'), '/');
  assert.equal(normalizePickerPath('/'), '/');
  assert.equal(normalizePickerPath('home/me'), '');
});

// 场景:「上一级」按钮与面包屑。期望:父目录逐级回退,盘根 / 文件系统根的父目录是 ''。
test('parent path stops at drive and filesystem roots', () => {
  assert.equal(pickerParentPath('C:/Users/shao'), 'C:/Users');
  assert.equal(pickerParentPath('C:/Users'), 'C:/');
  assert.equal(pickerParentPath('C:/'), '');
  assert.equal(pickerParentPath('/home/me'), '/home');
  assert.equal(pickerParentPath('/home'), '/');
  assert.equal(pickerParentPath('/'), '');
  assert.equal(pickerParentPath(''), '');
  assert.equal(isPickerRoot('C:/'), true);
  assert.equal(isPickerRoot('/'), true);
  assert.equal(isPickerRoot('C:/Users'), false);
});

test('breadcrumbs carry cumulative paths for both path families', () => {
  assert.deepEqual(pickerBreadcrumbs('C:/Users/shao'), [
    { label: 'C:', path: 'C:/' },
    { label: 'Users', path: 'C:/Users' },
    { label: 'shao', path: 'C:/Users/shao' },
  ]);
  assert.deepEqual(pickerBreadcrumbs('/home/me'), [
    { label: '/', path: '/' },
    { label: 'home', path: '/home' },
    { label: 'me', path: '/home/me' },
  ]);
  assert.deepEqual(pickerBreadcrumbs('C:/'), [{ label: 'C:', path: 'C:/' }]);
  assert.deepEqual(pickerBreadcrumbs(''), []);
  assert.equal(joinPickerPath('C:/', 'Users'), 'C:/Users');
  assert.equal(joinPickerPath('C:/Users', 'shao'), 'C:/Users/shao');
  assert.equal(joinPickerPath('/', 'home'), '/home');
});

// 场景:服务端按 ASCII 排过,但用户看到的应是「file2 在 file10 前」的自然序。
// 期望:目录始终在文件前,同类内数字感知、大小写不敏感。
test('sorts directories first with numeric-aware collation', () => {
  const sorted = sortPickerEntries([
    { name: 'file10.txt', kind: 'file' },
    { name: 'b', kind: 'dir' },
    { name: 'file2.txt', kind: 'file' },
    { name: 'A', kind: 'dir' },
  ]);
  assert.deepEqual(sorted.map((e) => e.name), ['A', 'b', 'file2.txt', 'file10.txt']);
});

// 场景:选文件夹模式下点到一个文件。期望:文件不可选(淡显),目录可选;
// 选文件模式下两者都可选(目录用来进入)。
test('selectability follows picker mode', () => {
  const dir = { name: 'src', kind: 'dir', path: 'C:/repo/src' };
  const file = { name: 'a.txt', kind: 'file', path: 'C:/repo/a.txt' };
  assert.equal(isSelectableEntry(dir, PICKER_MODE_FOLDER), true);
  assert.equal(isSelectableEntry(file, PICKER_MODE_FOLDER), false);
  assert.equal(isSelectableEntry(dir, PICKER_MODE_FILE), true);
  assert.equal(isSelectableEntry(file, PICKER_MODE_FILE), true);
  assert.deepEqual(selectableEntries([dir, file], PICKER_MODE_FOLDER), [dir]);
  assert.equal(isSelectableEntry(null, PICKER_MODE_FILE), false);
});

// 场景:主按钮该提交什么。期望:文件夹模式没选子项 → 当前目录;选中子目录 → 该目录;
// 处在根节点视图('')→ 不可用。文件模式只有选中文件才可用,选中目录不算。
test('selection target derives the confirm payload per mode', () => {
  assert.deepEqual(
    pickerSelectionTarget({ mode: PICKER_MODE_FOLDER, currentPath: 'C:/repo', selected: null }),
    { path: 'C:/repo', kind: 'dir', label: 'select-current', name: '' },
  );
  assert.deepEqual(
    pickerSelectionTarget({
      mode: PICKER_MODE_FOLDER,
      currentPath: 'C:/repo',
      selected: { name: 'src', kind: 'dir', path: 'C:/repo/src' },
    }),
    { path: 'C:/repo/src', kind: 'dir', label: 'select-named', name: 'src' },
  );
  assert.equal(pickerSelectionTarget({ mode: PICKER_MODE_FOLDER, currentPath: '', selected: null }), null);
  assert.equal(
    pickerSelectionTarget({ mode: PICKER_MODE_FILE, currentPath: 'C:/repo', selected: null }),
    null,
  );
  assert.equal(
    pickerSelectionTarget({
      mode: PICKER_MODE_FILE,
      currentPath: 'C:/repo',
      selected: { name: 'src', kind: 'dir', path: 'C:/repo/src' },
    }),
    null,
  );
  assert.deepEqual(
    pickerSelectionTarget({
      mode: PICKER_MODE_FILE,
      currentPath: 'C:/repo',
      selected: { name: 'a.txt', kind: 'file', path: 'C:/repo/a.txt' },
    }),
    { path: 'C:/repo/a.txt', kind: 'file', label: 'open-file', name: 'a.txt' },
  );
});

// 场景:在列表里直接打字。期望:优先名称前缀匹配,没有前缀命中再退化成子串;空缓冲区不匹配。
test('typeahead prefers prefix matches then falls back to substring', () => {
  const entries = [
    { name: 'docs', kind: 'dir' },
    { name: 'src', kind: 'dir' },
    { name: 'README.md', kind: 'file' },
  ];
  assert.equal(typeaheadMatch(entries, 'sr').name, 'src');
  assert.equal(typeaheadMatch(entries, 'read').name, 'README.md');
  assert.equal(typeaheadMatch(entries, 'oc').name, 'docs');
  assert.equal(typeaheadMatch(entries, ''), null);
  assert.equal(typeaheadMatch(entries, 'zzz'), null);
});

// 场景:弹窗起始目录。期望:调用方给的目录最优先;文件夹模式其次用上次确认的目录,再用主目录;
// 文件模式不看上次目录;都没有就回到根节点视图。
test('initial path prefers caller, then remembered folder, then home', () => {
  assert.equal(initialPickerPath({ mode: PICKER_MODE_FOLDER, initialPath: 'D:\\work', lastFolder: 'C:/old', home: 'C:/Users/me' }), 'D:/work');
  assert.equal(initialPickerPath({ mode: PICKER_MODE_FOLDER, lastFolder: 'C:/old', home: 'C:/Users/me' }), 'C:/old');
  assert.equal(initialPickerPath({ mode: PICKER_MODE_FOLDER, home: 'C:/Users/me' }), 'C:/Users/me');
  assert.equal(initialPickerPath({ mode: PICKER_MODE_FILE, lastFolder: 'C:/old', home: '/home/me' }), '/home/me');
  assert.equal(initialPickerPath({ mode: PICKER_MODE_FOLDER }), '');
  assert.equal(rememberedFolderAfterPick({ path: 'C:/repo/src', kind: 'dir' }), 'C:/repo/src');
  assert.equal(rememberedFolderAfterPick({ path: 'C:/repo/a.txt', kind: 'file' }), 'C:/repo');
  assert.equal(rememberedFolderAfterPick(null), '');
  assert.equal(validatePathPickerPrefs({ lastFolder: '' }), true);
  assert.equal(validatePathPickerPrefs({ lastFolder: 1 }), false);
  assert.equal(validatePathPickerPrefs(null), false);
});

// 场景:「此电脑」视图里的盘符行。期望:有卷标显示「卷标 (C:)」,没有卷标用兜底文案;
// 容量条按已用比例算,缺容量信息返回 null 而不是 NaN。
test('root rows render volume labels and usage percent defensively', () => {
  assert.equal(rootDisplayName({ path: 'C:/', label: 'Windows' }, 'Local Disk'), 'Windows (C:)');
  assert.equal(rootDisplayName({ path: 'd:/', label: '' }, 'Local Disk'), 'Local Disk (D:)');
  assert.equal(rootDisplayName({ path: '/', label: '' }, 'Root'), '/');
  assert.equal(rootDisplayName({ path: '/Volumes/Data', label: 'Data' }, 'Root'), 'Data');
  assert.equal(rootUsagePercent({ total_bytes: 1000, free_bytes: 250 }), 75);
  assert.equal(rootUsagePercent({ total_bytes: 0, free_bytes: 0 }), null);
  assert.equal(rootUsagePercent({}), null);
  assert.equal(browseErrorKey(403), 'permission-denied');
  assert.equal(browseErrorKey(404), 'not-found');
  assert.equal(browseErrorKey(400), 'not-absolute');
  assert.equal(browseErrorKey(500), 'io-error');
});

// 场景:新建文件夹前按平台判断同名比较规则。期望:Windows / macOS 默认卷不分大小写,Linux 区分;
// 撞名检查(newFolderNameProblem 的 caseInsensitive)由这里决定。
test('picker name comparison follows the platform case rule', () => {
  assert.equal(pickerNamesCaseInsensitive('windows'), true);
  assert.equal(pickerNamesCaseInsensitive('macos'), true);
  assert.equal(pickerNamesCaseInsensitive('linux'), false);
});

// 场景:用户在「新建文件夹」弹窗里点确定(或按 Enter)之前的本地检查。期望:与服务端 folder_name_problem 同一套
// 规则 —— 空名、. / ..、分隔符、控制字符所有平台都拒;<>:"|?*、句点或空格结尾、保留设备名
// (带扩展名、大小写不同也算)只在 Windows 规则下拒;与当前列表撞名报 exists(大小写规则跟平台)。
// 中文名、点开头、中间带空格都合法;COM0 / COM10 不是保留名。
test('new folder name problems mirror the server rules', () => {
  const backslash = String.fromCharCode(92);
  for (const windows of [true, false]) {
    assert.equal(newFolderNameProblem('', { windows }), 'empty');
    assert.equal(newFolderNameProblem('..', { windows }), 'dot');
    assert.equal(newFolderNameProblem('a/b', { windows }), 'separator');
    assert.equal(newFolderNameProblem(`a${backslash}b`, { windows }), 'separator');
    assert.equal(newFolderNameProblem(`a${String.fromCharCode(9)}b`, { windows }), 'control');
    assert.equal(newFolderNameProblem('新建文件夹 (2)', { windows }), '');
    assert.equal(newFolderNameProblem('.config', { windows }), '');
    assert.equal(newFolderNameProblem('COM10', { windows }), '');
  }
  for (const bad of ['a:b', 'a*b', 'a?b', 'a"b', 'a<b', 'a|b']) {
    assert.equal(newFolderNameProblem(bad, { windows: true }), 'windows-char', bad);
    assert.equal(newFolderNameProblem(bad, { windows: false }), '', bad);
  }
  assert.equal(newFolderNameProblem('trailing.', { windows: true }), 'trailing');
  assert.equal(newFolderNameProblem('trailing ', { windows: true }), 'trailing');
  assert.equal(newFolderNameProblem('con.txt', { windows: true }), 'reserved');
  assert.equal(newFolderNameProblem('Lpt9', { windows: true }), 'reserved');
  assert.equal(newFolderNameProblem('COM0', { windows: true }), '');
  assert.equal(newFolderNameProblem('CON', { windows: false }), '');

  // 弹窗输入框限 50 个字:正好 50 合法,51 报 too-long(输入框 maxLength 挡不住输入法合成等情况时兜底)。
  assert.equal(NEW_FOLDER_NAME_MAX_LENGTH, 50);
  assert.equal(newFolderNameProblem('文'.repeat(50), { windows: true }), '');
  assert.equal(newFolderNameProblem('文'.repeat(51), { windows: true }), 'too-long');
  assert.equal(newFolderNameProblem('a'.repeat(51), { windows: false }), 'too-long');

  const entries = [{ name: 'Docs', kind: 'dir' }];
  assert.equal(newFolderNameProblem('docs', { windows: true, entries }), 'exists');
  assert.equal(newFolderNameProblem('docs', { windows: false, entries }), '');
  assert.equal(newFolderNameProblem('Docs', { windows: false, entries }), 'exists');
});

// 场景:POST /api/fs/mkdir 失败。期望:409 → 已存在;403 → 无权限;404 → 所在目录不见了;
// 400 只有 error="invalid name" 才算名字问题(其它 400 是路径问题,归 io-error 兜底文案)。
test('mkdir failures map to inline message keys', () => {
  assert.equal(mkdirErrorKey(409, { error: 'already exists' }), 'exists');
  assert.equal(mkdirErrorKey(403, {}), 'permission-denied');
  assert.equal(mkdirErrorKey(404, { error: 'not found' }), 'parent-missing');
  assert.equal(mkdirErrorKey(400, { error: 'invalid name' }), 'invalid-name');
  assert.equal(mkdirErrorKey(400, { error: 'path must be absolute' }), 'io-error');
  assert.equal(mkdirErrorKey(500, null), 'io-error');
});
