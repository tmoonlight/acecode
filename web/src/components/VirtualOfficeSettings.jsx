import { Toggle } from './Modal.jsx';

export function VirtualOfficeSettings({ office }) {
  return (
    <section>
      <h2 className="text-xl font-bold mb-5">虚拟办公室</h2>
      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-normal text-fg">开启虚拟办公室</div>
          <div className="text-[11px] text-fg-mute mt-0.5">
            {office?.available ? '打开虚拟办公室，为您的工作增添更多乐趣！' : '虚拟办公室支持 Windows 和 macOS 桌面版。'}
          </div>
        </div>
        <Toggle ariaLabel="开启虚拟办公室" on={!!office?.enabled}
          disabled={!office?.available || office.busy} onChange={office?.setEnabled} />
      </div>
      {office?.error && <p role="alert" className="text-[12px] text-danger mt-2">{office.error}</p>}
    </section>
  );
}
