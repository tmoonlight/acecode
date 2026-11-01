import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { DESKTOP_PET_KINDS } from '../lib/desktopPetKinds.js';
import clippyPreview from '../../../assets/desktop_pet/classic/clippy/preview.png';
import roverPreview from '../../../assets/desktop_pet/classic/rover/preview.png';
import whalePreview from '../../../assets/desktop_pet/whale/preview.png';

export function VirtualOfficeSettings({ office }) {
  return (
    <section>
      <h2 className="text-xl font-bold mb-5">桌面宠物</h2>
      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-normal text-fg">开启桌面宠物</div>
          {!office?.available && <div className="text-[11px] text-fg-mute mt-0.5">桌面宠物支持 Windows 和 macOS 桌面版。</div>}
        </div>
        <Toggle ariaLabel="开启桌面宠物" on={!!office?.enabled}
          disabled={!office?.available || office.busy} onChange={office?.setEnabled} />
      </div>
      <div className="mt-5 grid grid-cols-2 gap-3 max-w-2xl sm:grid-cols-4" role="radiogroup" aria-label="选择桌面宠物">
        {DESKTOP_PET_KINDS.map(pet => {
          const selected = (office?.petKind || 'office') === pet.id;
          const preview = pet.id === 'clippy' ? clippyPreview : pet.id === 'rover' ? roverPreview : pet.id === 'whale' ? whalePreview : '';
          return <button key={pet.id} type="button" role="radio" aria-checked={selected}
            disabled={!office?.available || office.busy} onClick={() => office?.setPetKind?.(pet.id)}
            className={`relative p-3 rounded-lg border text-left transition disabled:opacity-60 focus-visible:outline-2 focus-visible:outline-accent focus-visible:outline-offset-2 ${selected ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:bg-surface-hi'}`}>
            <div className="h-24 mb-3 flex items-center justify-center" aria-hidden="true">
              {pet.id === 'mini' ? <div className="flex items-center w-full max-w-[118px] rounded-full bg-bg shadow-md px-1 py-2.5">
                {['M10 3H6a3 3 0 0 0-3 3v12a3 3 0 0 0 3 3h12a3 3 0 0 0 3-3v-4M12 13l-4 1 1-4 9-8a2 2 0 0 1 3 3Z',
                  'M5 10v4m4-9v14m4-11v8m4-6v4m3-3v2',
                  'M9 20a3 3 0 0 0 6 0M19 9a7 7 0 0 0-14 0c0 7-2 7-2 9h18c0-2-2-2-2-9Z'].map((path, index) => <span key={index}
                  className={`flex flex-1 min-w-0 items-center justify-center px-1 ${index ? 'border-l border-border' : ''}`}>
                  <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.85" strokeLinecap="round" strokeLinejoin="round"><path d={path} /></svg>
                </span>)}
              </div> : preview ? <img src={preview} alt="" className="max-w-full max-h-full object-contain" />
                : <VsIcon name={pet.id === 'office' ? 'office' : 'smiley'} size={48} className="text-fg-mute" />}
            </div>
            <div className="text-[13px] font-normal text-fg">{pet.label}</div>
            {selected && <span className="absolute top-2 right-2 w-2 h-2 rounded-full bg-accent" />}
          </button>;
        })}
      </div>
      {office?.error && <p role="alert" className="text-[12px] text-danger mt-2">{office.error}</p>}
    </section>
  );
}
