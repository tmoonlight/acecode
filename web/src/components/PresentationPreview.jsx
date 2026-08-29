import { useEffect, useMemo, useRef, useState } from 'react';
import rawRendererSource from 'pptx2html/dist/pptx2html.full.min.js?raw';
import {
  createPresentationPreviewChannel,
  postPresentationPreviewMessage,
  presentationFrameDocument,
  presentationPreviewMessage,
  presentationRendererSource,
} from '../lib/presentationPreview.js';
import {
  OfficePreviewControls,
  useOfficePreviewZoom,
} from './OfficePreviewControls.jsx';
import { VsIcon } from './Icon.jsx';

const RENDERER_READY_TIMEOUT_MS = 10_000;
const rendererSource = presentationRendererSource(rawRendererSource);

export function PresentationPreview({ blob, path }) {
  const frameRef = useRef(null);
  const shellRef = useRef(null);
  const [frameRevision, setFrameRevision] = useState(0);
  const [status, setStatus] = useState('loading');
  const [error, setError] = useState('');
  const [slideIndex, setSlideIndex] = useState(0);
  const [slideCount, setSlideCount] = useState(0);
  const {
    zoom,
    setZoom,
    zoomIn,
    zoomOut,
  } = useOfficePreviewZoom(path, shellRef);
  const channel = useMemo(() => createPresentationPreviewChannel(), [blob, path]);
  const srcDoc = useMemo(
    () => presentationFrameDocument(rendererSource, channel),
    [channel],
  );

  useEffect(() => {
    const frameWindow = frameRef.current?.contentWindow;
    if (!blob || !frameWindow) return undefined;
    let cancelled = false;
    let rendering = false;
    setStatus('loading');
    setError('');
    setSlideIndex(0);
    setSlideCount(0);

    const timeoutId = window.setTimeout(() => {
      if (cancelled || rendering) return;
      setStatus('error');
      setError('pptx2html renderer unavailable');
    }, RENDERER_READY_TIMEOUT_MS);

    const receiveMessage = async (event) => {
      const message = presentationPreviewMessage(event, frameWindow, channel);
      if (!message || cancelled) return;
      if (Number.isInteger(message.slideIndex)) setSlideIndex(message.slideIndex);
      if (Number.isInteger(message.slideCount)) setSlideCount(message.slideCount);
      if (Number.isFinite(message.zoom)) setZoom(message.zoom);
      if (message.status === 'error') {
        window.clearTimeout(timeoutId);
        setStatus('error');
        setError(message.detail || '读取失败');
        return;
      }
      if (message.status === 'complete') {
        window.clearTimeout(timeoutId);
        setStatus('complete');
        return;
      }
      if (message.status !== 'ready' || rendering) return;
      rendering = true;
      window.clearTimeout(timeoutId);
      try {
        const buffer = await blob.arrayBuffer();
        if (cancelled) return;
        postPresentationPreviewMessage(
          frameWindow,
          channel,
          'render',
          { buffer },
          [buffer],
        );
      } catch (nextError) {
        if (cancelled) return;
        setStatus('error');
        setError(nextError?.message || '读取失败');
      }
    };

    window.addEventListener('message', receiveMessage);
    postPresentationPreviewMessage(frameWindow, channel, 'ping');
    return () => {
      cancelled = true;
      window.clearTimeout(timeoutId);
      window.removeEventListener('message', receiveMessage);
    };
  }, [blob, channel, frameRevision, setZoom]);

  useEffect(() => {
    if (status !== 'complete') return;
    postPresentationPreviewMessage(
      frameRef.current?.contentWindow,
      channel,
      'zoom',
      { zoom },
    );
  }, [channel, frameRevision, status, zoom]);

  const navigate = (delta) => {
    postPresentationPreviewMessage(
      frameRef.current?.contentWindow,
      channel,
      'navigate',
      { delta },
    );
  };

  return (
    <div
      ref={shellRef}
      className="ace-office-preview-shell ace-side-presentation-preview"
      aria-busy={status === 'loading' ? 'true' : undefined}
    >
      <iframe
        ref={frameRef}
        className={status === 'error' ? 'ace-side-presentation-frame is-hidden' : 'ace-side-presentation-frame'}
        title={`PowerPoint preview: ${path}`}
        sandbox="allow-scripts"
        srcDoc={srcDoc}
        onLoad={() => setFrameRevision((revision) => revision + 1)}
      />
      {status === 'complete' && slideCount > 0 && (
        <>
          <button
            type="button"
            className="ace-side-presentation-nav is-previous"
            title="上一张幻灯片"
            aria-label="上一张幻灯片"
            disabled={slideIndex <= 0}
            onClick={() => navigate(-1)}
          >
            <span className="ace-side-presentation-nav-icon">
              <VsIcon name="arrowLeft" size={26} />
            </span>
          </button>
          <button
            type="button"
            className="ace-side-presentation-nav is-next"
            title="下一张幻灯片"
            aria-label="下一张幻灯片"
            disabled={slideIndex >= slideCount - 1}
            onClick={() => navigate(1)}
          >
            <span className="ace-side-presentation-nav-icon">
              <VsIcon name="arrowRight" size={26} />
            </span>
          </button>
        </>
      )}
      {status === 'complete' && (
        <OfficePreviewControls
          zoom={zoom}
          onZoomIn={zoomIn}
          onZoomOut={zoomOut}
        />
      )}
      {status === 'loading' && (
        <div className="ace-side-presentation-status">加载中...</div>
      )}
      {status === 'error' && (
        <div className="ace-empty-state ace-side-presentation-status">
          <div className="text-danger text-[12px] mb-1">{error || '读取失败'}</div>
          <div className="text-fg-mute text-[10px] opacity-70 break-all">{path}</div>
        </div>
      )}
    </div>
  );
}
