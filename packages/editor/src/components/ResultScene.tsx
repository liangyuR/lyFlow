import { useEffect, useMemo, useState } from 'react';
import {
  poseCollection,
  poseScene2D,
  renderSvg,
  structuredScene,
  type SceneSpec,
} from '@lyflow/visualization';
import { exportCanvasPng } from '../lib/exportPng';
import type { OutputValue } from '../types/execution';

export function ResultScene({ value }: { value: OutputValue }) {
  const [selectedId, setSelectedId] = useState('');
  const [axes, setAxes] = useState(true);
  const [labels, setLabels] = useState(true);
  const [zoom, setZoom] = useState(1);
  useEffect(() => setSelectedId(''), [value]);
  const result = useMemo(() => {
    try {
      const poses = poseCollection(value);
      const scene =
        structuredScene(value) ??
        (poses?.dimension === 2
          ? poseScene2D(value, { selectedId, showAxes: axes, showLabels: labels })
          : null);
      return { poses, scene, svg: scene ? renderSvg(scene) : '', error: '' };
    } catch (error) {
      return { poses: null, scene: null, svg: '', error: String(error) };
    }
  }, [value, selectedId, axes, labels]);
  const [url, setUrl] = useState('');
  useEffect(() => {
    if (!result.svg) {
      setUrl('');
      return;
    }
    const next = URL.createObjectURL(new Blob([result.svg], { type: 'image/svg+xml' }));
    setUrl(next);
    return () => URL.revokeObjectURL(next);
  }, [result.svg]);
  if (result.error) return <p role="alert">{result.error}</p>;
  if (!result.scene) return null;
  const scene: SceneSpec = result.scene;
  const png = async () => {
    const image = new Image();
    image.src = url;
    await image.decode();
    const canvas = document.createElement('canvas');
    canvas.width = image.naturalWidth;
    canvas.height = image.naturalHeight;
    canvas.getContext('2d')!.drawImage(image, 0, 0);
    await exportCanvasPng(canvas, 'result-scene');
  };
  return (
    <section
      className="result-scene"
      data-testid="result-scene"
      tabIndex={0}
      onKeyDown={(event) => {
        if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
          event.preventDefault();
          setZoom((value) =>
            Math.max(0.25, Math.min(4, value * (event.key === 'ArrowUp' ? 1.25 : 0.8))),
          );
        }
      }}
    >
      <div className="viewer__bar">
        <span>
          {scene.frame.id} · {scene.frame.unit}
        </span>
        <button
          className="viewer__btn"
          type="button"
          aria-label="缩小结构化结果"
          onClick={() => setZoom((value) => Math.max(0.25, value / 1.25))}
        >
          −
        </button>
        <button className="viewer__btn" type="button" onClick={() => setZoom(1)}>
          {Math.round(zoom * 100)}%
        </button>
        <button
          className="viewer__btn"
          type="button"
          aria-label="放大结构化结果"
          onClick={() => setZoom((value) => Math.min(4, value * 1.25))}
        >
          +
        </button>
        {result.poses && (
          <>
            <select
              aria-label="工件身份"
              value={selectedId}
              onChange={(event) => setSelectedId(event.target.value)}
            >
              <option value="">全部 {result.poses.items.length}</option>
              {result.poses.items.map((item) => (
                <option key={item.id} value={item.id}>
                  {item.name || item.id} · {item.id}
                </option>
              ))}
            </select>
            <label>
              <input
                type="checkbox"
                checked={axes}
                onChange={(event) => setAxes(event.target.checked)}
              />
              方向
            </label>
            <label>
              <input
                type="checkbox"
                checked={labels}
                onChange={(event) => setLabels(event.target.checked)}
              />
              标签
            </label>
          </>
        )}
        <a className="viewer__btn" href={url} download="result-scene.svg">
          SVG
        </a>
        <button className="viewer__btn" type="button" onClick={() => void png()}>
          PNG
        </button>
      </div>
      <div className="result-scene__viewport">
        <img
          src={url}
          style={{ width: `${zoom * 100}%` }}
          alt={`结构化结果 ${scene.frame.id}，单位 ${scene.frame.unit}`}
        />
      </div>
    </section>
  );
}
