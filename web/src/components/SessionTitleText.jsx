// The complete title reserves width and remains available to assistive technology.
export function SessionTitleText({ title, animation }) {
  const typing = Boolean(animation?.typing);
  return (
    <span className="ace-session-title-transition" data-title-typing={typing ? 'true' : 'false'}>
      <span className="ace-session-title-complete">{title}</span>
      {typing && (
        <span className="ace-session-title-typing" aria-hidden="true">
          {animation.text}<span className="ace-session-title-cursor" />
        </span>
      )}
    </span>
  );
}
