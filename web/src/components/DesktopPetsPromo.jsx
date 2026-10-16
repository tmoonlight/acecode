import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { DESKTOP_PET_KINDS } from '../lib/desktopPetKinds.js';
import clippyPreview from '../../../assets/desktop_pet/classic/clippy/preview.png';
import roverPreview from '../../../assets/desktop_pet/classic/rover/preview.png';
import whalePreview from '../../../assets/desktop_pet/whale/preview.png';

const PREVIEWS = { clippy: clippyPreview, rover: roverPreview, whale: whalePreview };
// The companion pets added after the virtual office, in Settings order.
const NEW_PETS = DESKTOP_PET_KINDS.filter(pet => PREVIEWS[pet.id]);

// One-time announcement of the companion pets: a "新特性" title, one subtitle line and the pets,
// no explanations (user decision).
// Picking one switches to it, turning desktop pets on if they were off.
export function DesktopPetsPromo({ office, onClose }) {
  const close = () => { if (!office.busy) onClose(); };
  const choose = async kind => {
    if (office.busy) return;
    if (office.petKind !== kind && !(await office.setPetKind(kind)).ok) return;
    if (!office.enabled && !(await office.setEnabled(true)).ok) return;
    onClose();
  };
  return (
    <Modal width={520} onClose={close} dismissOnBackdrop={false} labelledBy="pets-promo-title">
      <div className="p-5">
        <div className="flex items-center justify-between gap-3">
          <h2 id="pets-promo-title" className="text-xl font-bold">新特性</h2>
          <button type="button" aria-label="关闭" disabled={office.busy} onClick={close}
            className="w-8 h-8 shrink-0 flex items-center justify-center rounded-md text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-60">
            <VsIcon name="close" size={18} />
          </button>
        </div>
        <p className="text-[14px] text-fg-2 mt-1.5">增加更多宠物，丰富您快乐工作的每一天</p>
        <div className="mt-5 grid grid-cols-3 gap-3">
          {NEW_PETS.map(pet => {
            const current = office.enabled && office.petKind === pet.id;
            return <button key={pet.id} type="button" aria-pressed={current} disabled={office.busy}
              onClick={() => choose(pet.id)}
              className={`p-3 rounded-lg border transition disabled:opacity-60 focus-visible:outline-2 focus-visible:outline-accent focus-visible:outline-offset-2 ${current ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:bg-surface-hi'}`}>
              <div className="h-28 mb-2 flex items-center justify-center" aria-hidden="true">
                <img src={PREVIEWS[pet.id]} alt="" className="max-w-full max-h-full object-contain" />
              </div>
              <div className="text-[13px] text-fg text-center">{pet.label}</div>
            </button>;
          })}
        </div>
        {office.error && <p role="alert" className="text-[12px] text-danger mt-3">{office.error}</p>}
        <div className="flex justify-end mt-5 text-[13px]">
          <button type="button" onClick={close} disabled={office.busy} data-ace-dialog-primary
            className="px-3 py-1.5 bg-accent text-white rounded disabled:opacity-60">知道了</button>
        </div>
      </div>
    </Modal>
  );
}
