import React from 'react';
import { VsIcon } from './Icon.jsx';
import { DEFAULT_HOME_INTENT_PROMPTS, HOME_INTENT_CATEGORIES } from '../lib/homeIntentPrompts.js';

export function HomeIntentPromptCards({ suggestions = [], onSelect, disabled = false }) {
  const cards = suggestions.length ? suggestions : DEFAULT_HOME_INTENT_PROMPTS;
  return (
    <div className="ace-home-hints" aria-label="任务建议">
      {cards.map((card, index) => {
        const category = HOME_INTENT_CATEGORIES[card.category];
        return (
          <button key={`${card.category}:${index}`} type="button" className="ace-home-hint-card"
            data-home-intent-category={card.category} disabled={disabled} onClick={() => onSelect?.(card)}>
            <div className="flex items-center gap-2 mb-1 min-w-0">
              <VsIcon name={category.icon} size={22} />
              {card.workspace && <span className="ace-home-hint-workspace text-[11px] text-fg-mute"
                title={card.workspace.cwd || '无工作区'}>
                {card.workspace.noWorkspace ? '无工作区' : card.workspace.name || card.workspace.cwd}
              </span>}
            </div>
            <div className="text-[13px] font-semibold">{card.title}</div>
            {card.description && <div className="mt-0.5 text-[11px] text-fg-mute leading-relaxed">{card.description}</div>}
          </button>
        );
      })}
    </div>
  );
}
