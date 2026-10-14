// 每轮对话末尾的「本轮改动文件」列表(Claude Code 风格):
// 标题行「已修改 xx 个文件 +X -Y」+ 行卡(文件树同款类型 icon + 单一路径
//(cwd 内相对 / cwd 外绝对)+ 红绿加删数 + 「打开」)。整行打开文件内容;
// 悬停后行数位置显示独立的「查看变更」入口,打开该轮 diff。文件数超过阈值折叠,
// 「展开查看剩余 x 个文件」/「收起」切换。
//
// 纯逻辑(路径展示 / 条目构建 / 折叠切分)在 lib/turnFileList.js,有 Node 单测。

import { memo, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { buildTurnFileItems, splitTurnFileItems } from '../lib/turnFileList.js';
import { summarizeChangeGroups } from '../lib/sessionChanges.js';
import { formatCount } from '../lib/format.js';
import { FileTypeIcon, VsIcon } from './Icon.jsx';

function ChangeCounts({ additions, deletions, className = '' }) {
  if (!(additions > 0) && !(deletions > 0)) return null;
  return (
    <span className={`ace-turn-file-counts ${className}`.trim()}>
      {additions > 0 && <span className="ace-change-add">+{additions}</span>}
      {deletions > 0 && <span className="ace-change-del">-{deletions}</span>}
    </span>
  );
}

export const TurnFileList = memo(function TurnFileList({
  groups,
  summary,
  cwd = '',
  turnUserMessageId = '',
  onOpenChanges,
  onOpenFile,
}) {
  useTranslation();
  const [expanded, setExpanded] = useState(false);
  const items = useMemo(() => buildTurnFileItems(groups, cwd), [groups, cwd]);
  const changeSummary = summary && typeof summary === 'object'
    ? summary
    : summarizeChangeGroups(groups);
  const { visible, hiddenCount, collapsible } = splitTurnFileItems(items, expanded);
  if (items.length === 0) return null;

  return (
    <div className="ace-turn-files" data-chat-turn-files="true">
      <div className="ace-turn-files-title">
        <span>{formatCount(changeSummary.fileCount, 'filesModified')}</span>
        <ChangeCounts
          additions={changeSummary.totalAdditions}
          deletions={changeSummary.totalDeletions}
        />
      </div>
      {visible.map((item) => (
        <div
          key={item.file}
          className="ace-turn-file-row"
        >
          <button
            type="button"
            className="ace-turn-file-target"
            onClick={() => onOpenFile?.(item.openPath)}
            title={item.openPath}
          >
            <span className="sr-only"><span>打开文件</span> {item.displayPath}</span>
          </button>
          <FileTypeIcon path={item.file} size={16} className="ace-turn-file-icon" />
          <span className="ace-turn-file-path" aria-hidden="true">{item.displayPath}</span>
          <span className="ace-turn-file-detail">
            <ChangeCounts additions={item.additions} deletions={item.deletions} />
            <button
              type="button"
              className="ace-turn-file-changes"
              onClick={() => onOpenChanges?.(item.file, turnUserMessageId)}
            >
              <span>查看变更</span>
              <VsIcon name="ArrowUpRight" size={12} />
            </button>
          </span>
          <span className="ace-turn-file-open" aria-hidden="true">打开</span>
        </div>
      ))}
      {collapsible && (
        <button
          type="button"
          className="ace-turn-files-toggle"
          onClick={() => setExpanded((v) => !v)}
        >
          {expanded ? '收起' : `展开查看剩余 ${hiddenCount} 个文件`}
        </button>
      )}
    </div>
  );
});
