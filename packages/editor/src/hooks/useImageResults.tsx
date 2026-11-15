import { useEffect, useMemo, useRef, useState } from 'react';
import {
  cameraProjector,
  cloudGroupPreview,
  maskPreview,
  poseCollection,
  poseScene2D,
  projectedPoseScene,
  renderImageOverlaySvg,
  type SceneSpec,
  type ResultOptions,
} from '@lyflow/visualization';
import { decodeCloud, decodeIndices, decodeTensor, type OutputStat } from '../types/execution';
import { transport } from '../transport';
import { useResultBindings } from './useResultBindings';
import { useRuntimeCapabilities } from './useRuntimeCapabilities';
import { exportCanvasPng } from '../lib/exportPng';
import type { ImageOverlayView } from '../components/ImageCanvas';

export function useImageResults(
  runId: string | null,
  nodeId: string,
  outputs: readonly OutputStat[] | undefined,
  width: number,
  height: number,
) {
  const related = useResultBindings(runId);
  const capabilities = useRuntimeCapabilities();
  const records = useMemo(() => {
    const result = related.outputs.filter((output) => output.type === 'Record');
    for (const output of outputs ?? [])
      if (
        output.type === 'Record' &&
        !result.some((item) => item.nodeId === nodeId && item.port === output.port)
      )
        result.push({ ...output, byteSize: 0, nodeId });
    return result;
  }, [related.outputs, outputs, nodeId]);
  const poses = records.filter((output) =>
    /^vision\.Poses?[23]D\.v1$/.test(output.value?.type ?? ''),
  );
  const cameras = records.filter((output) => output.value?.type === 'vision.CameraCalib.v1');
  const cloudGroups = records.filter((output) => output.value?.type === 'cloud.Groups.v1');
  const [posePick, setPosePick] = useState('');
  const [cameraPick, setCameraPick] = useState('');
  const [groupPick, setGroupPick] = useState('');
  const [groupId, setGroupId] = useState('');
  const [groupIndexPort, setGroupIndexPort] = useState('');
  const [instance, setInstance] = useState('');
  const [frame, setFrame] = useState('');
  const [labels, setLabels] = useState(true);
  const [axes, setAxes] = useState(true);
  const [maskAlpha, setMaskAlpha] = useState(0.35);
  const [lineWidth, setLineWidth] = useState(2);
  const [colorBy, setColorBy] = useState<'id' | 'class'>('id');
  const [fixedColor, setFixedColor] = useState('');
  const [transformText, setTransformText] = useState('');
  const [maskUrl, setMaskUrl] = useState('');
  const [cloudLayer, setCloudLayer] = useState<SceneSpec['layers'][number] | null>(null);
  const [loadError, setLoadError] = useState('');
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  useEffect(() => {
    setPosePick('');
    setCameraPick('');
    setGroupPick('');
    setGroupId('');
    setGroupIndexPort('');
    setInstance('');
    setFrame('');
    setTransformText('');
  }, [nodeId]);
  const identity = (output: { nodeId: string; port: string }) => `${output.nodeId}:${output.port}`;
  const poseOutput = poses.find((output) => identity(output) === posePick);
  const camera = cameras.find((output) => identity(output) === cameraPick);
  const group = cloudGroups.find((output) => identity(output) === groupPick);
  const groupItems = group?.value?.data?.groups;
  const groupNames = Array.isArray(groupItems)
    ? groupItems.filter((item): item is { id: string } => item && typeof item.id === 'string')
    : [];
  const parsed = useMemo(() => {
    try {
      return { poses: poseCollection(poseOutput?.value), error: '' };
    } catch (error) {
      return { poses: null, error: String(error) };
    }
  }, [poseOutput]);
  const parsedTransform = useMemo(() => {
    try {
      return { value: transformText ? JSON.parse(transformText) : undefined, error: '' };
    } catch (error) {
      return { value: undefined, error: String(error) };
    }
  }, [transformText]);
  const options = useMemo<ResultOptions & { error: string }>(
    () => ({
      selectedId: instance || null,
      showLabels: labels,
      showAxes: axes,
      lineWidth,
      colorBy,
      ...(fixedColor ? { color: fixedColor } : {}),
      ...(parsedTransform.value ? { transform: parsedTransform.value } : {}),
      error: parsedTransform.error,
    }),
    [instance, labels, axes, lineWidth, colorBy, fixedColor, parsedTransform],
  );
  useEffect(() => {
    setMaskUrl('');
    setCloudLayer(null);
    setLoadError('');
    if (!runId || width < 1 || height < 1) return;
    let cancelled = false;
    void (async () => {
      try {
        const masks = records.filter(
          (output) =>
            output.nodeId === poseOutput?.nodeId && output.value?.type === 'vision.Masks2D.v1',
        );
        const tensors = related.outputs.filter(
          (output) => output.nodeId === poseOutput?.nodeId && output.type === 'Tensor',
        );
        if (masks.length && (masks.length !== 1 || tensors.length !== 1))
          throw new Error('掩膜需要唯一配套的元数据与 Tensor，请先明确输出节点');
        if (poseOutput && masks.length === 1 && tensors.length === 1) {
          if (masks[0]!.value?.data?.width !== width || masks[0]!.value?.data?.height !== height)
            throw new Error('掩膜分辨率不匹配当前原图，预览缩放结果需正式运行');
          if (tensors[0]!.elementCount > 16_777_216) throw new Error('掩膜超过 16777216 像素预算');
          const tensor = decodeTensor(
            await transport.getOutputTensor(
              runId,
              tensors[0]!.nodeId,
              tensors[0]!.port,
              0,
              tensors[0]!.elementCount,
            ),
          );
          if (tensor.count !== tensor.total || tensor.offset !== 0)
            throw new Error('掩膜张量不完整');
          const result = maskPreview(masks[0]!.value, tensor, poseOutput.value, {
            selectedId: instance || null,
            alpha: maskAlpha,
            colorBy,
            ...(fixedColor ? { color: fixedColor } : {}),
          });
          if (result.width !== width || result.height !== height)
            throw new Error('掩膜分辨率不匹配当前原图，预览缩放结果需正式运行');
          const canvas = document.createElement('canvas');
          canvas.width = width;
          canvas.height = height;
          const data = canvas.getContext('2d')!.createImageData(width, height);
          data.data.set(result.pixels);
          canvas.getContext('2d')!.putImageData(data, 0, 0);
          if (!cancelled) setMaskUrl(canvas.toDataURL('image/png'));
        }
        if (group && camera) {
          if (capabilities.cloudSampling !== 'ordered-stride-v1')
            throw new Error('宿主不支持可证明的点云抽样身份');
          const native = related.outputs.find(
            (output) =>
              output.type === 'PointCloud' &&
              output.sourceCloudId === group.value!.data!.sourceCloudId,
          );
          let indexOutputs = related.outputs.filter(
            (output) => output.type === 'Indices' && output.nodeId === group.nodeId,
          );
          if (indexOutputs.length > 1)
            indexOutputs = indexOutputs.filter((output) => output.port === groupIndexPort);
          if (!native || indexOutputs.length !== 1)
            throw new Error(
              '投影需要唯一配套的原始点云与 Indices；请先使用 cloud.group_subset 明确分组',
            );
          const cloud = decodeCloud(
            await transport.getOutputCloud(runId, native.nodeId, native.port, 100_000),
          );
          const indexOutput = indexOutputs[0]!;
          if (indexOutput.elementCount > 16_777_216) throw new Error('点云分组索引预算超限');
          const indices = decodeIndices(
            await transport.getOutputIndices(
              runId,
              indexOutput.nodeId,
              indexOutput.port,
              0,
              indexOutput.elementCount,
            ),
          );
          if (indices.count !== indices.total) throw new Error('分组索引不完整');
          const paint = cloudGroupPreview(
            group.value,
            indices,
            {
              ...cloud,
              sourceCloudId: native.sourceCloudId!,
              sampleStride: Math.max(1, Math.ceil(cloud.totalPoints / 100_000)),
            },
            { selectedId: groupId || null },
          );
          const projector = cameraProjector(camera.value, paint.frame, options.transform);
          if (
            projector.imageSize[0] !== width ||
            projector.imageSize[1] !== height ||
            frame !== projector.frame
          )
            throw new Error('相机、图像坐标系或分辨率不一致');
          const points: [number, number][] = [],
            colors: string[] = [];
          for (let i = 0; i < cloud.pointCount; i++) {
            if (!paint.visible[i]) continue;
            const point = projector.project(Array.from(cloud.xyz.subarray(i * 3, i * 3 + 3)));
            if (!point || point[0] < 0 || point[0] >= width || point[1] < 0 || point[1] >= height)
              continue;
            points.push(point);
            colors.push(
              `#${Array.from(paint.colors.subarray(i * 3, i * 3 + 3), (v) => v.toString(16).padStart(2, '0')).join('')}`,
            );
          }
          if (!cancelled)
            setCloudLayer({
              id: 'projected-cloud',
              label: `投影显示 ${cloud.pointCount}/${cloud.totalPoints} 点`,
              kind: 'points',
              frame: { id: frame, unit: 'px', axes: ['u', 'v'], yDirection: 'down' },
              points,
              colors,
              totalPoints: cloud.totalPoints,
            });
        }
      } catch (error) {
        if (!cancelled) setLoadError(String(error));
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [
    runId,
    records,
    related.outputs,
    poseOutput,
    group,
    camera,
    width,
    height,
    instance,
    groupId,
    groupIndexPort,
    maskAlpha,
    frame,
    options.transform,
    capabilities.cloudSampling,
    colorBy,
    fixedColor,
  ]);
  const render = useMemo(() => {
    try {
      if (options.error) throw new Error(options.error);
      if (!frame || width < 1 || height < 1) return { svg: '', error: '' };
      let scene: SceneSpec = {
        schemaVersion: 1,
        frame: { id: frame, unit: 'px', axes: ['u', 'v'], yDirection: 'down' },
        layers: [],
      };
      if (parsed.poses && poseOutput) {
        if (parsed.poses.dimension === 2) {
          if (parsed.poses.unit !== 'px' || parsed.poses.frame !== frame)
            throw new Error('二维工件与图像必须属于同一像素坐标系');
          scene = poseScene2D(poseOutput.value, options);
        } else {
          if (!camera) throw new Error('三维叠图需要相机标定；跨坐标系还需要明确外参');
          scene = projectedPoseScene(poseOutput.value, camera.value, options);
          if (
            scene.frame.id !== frame ||
            camera.value!.data!.imageSize?.toString() !== [width, height].toString()
          )
            throw new Error('相机与图像的坐标系或分辨率不匹配');
        }
      }
      if (maskUrl && parsed.poses?.frame === frame)
        scene.layers.unshift({
          id: 'instance-masks',
          kind: 'image',
          frame: scene.frame,
          dataUrl: maskUrl,
          bounds: [0, 0, width, height],
        });
      if (cloudLayer) scene.layers.push(cloudLayer);
      return { svg: renderImageOverlaySvg(scene, width, height), error: '' };
    } catch (error) {
      return { svg: '', error: String(error) };
    }
  }, [frame, width, height, parsed, poseOutput, camera, options, maskUrl, cloudLayer]);
  const [url, setUrl] = useState('');
  useEffect(() => {
    if (!render.svg) {
      setUrl('');
      return;
    }
    const next = URL.createObjectURL(new Blob([render.svg], { type: 'image/svg+xml' }));
    setUrl(next);
    return () => URL.revokeObjectURL(next);
  }, [render.svg]);
  const exportPng = async () => {
    if (!canvasRef.current) return;
    const canvas = document.createElement('canvas');
    canvas.width = canvasRef.current.width;
    canvas.height = canvasRef.current.height;
    canvas.getContext('2d')!.drawImage(canvasRef.current, 0, 0);
    if (url) {
      const image = new Image();
      image.src = url;
      await image.decode();
      canvas.getContext('2d')!.drawImage(image, 0, 0, canvas.width, canvas.height);
    }
    await exportCanvasPng(canvas, 'image-result');
  };
  const toolbar =
    poses.length || cloudGroups.length ? (
      <details
        className="viewer__result-tools viewer__result-tools--popover"
        data-testid="image-result-tools"
      >
        <summary>
          结果叠加{poseOutput ? ' · 工件' : ''}
          {group ? ' · 点云' : ''}
        </summary>
        <div className="viewer__result-panel">
          <label>
            工件
            <select
              aria-label="叠加工件"
              value={posePick}
              onChange={(event) => {
                setPosePick(event.target.value);
                setInstance('');
              }}
            >
              <option value="">不叠加</option>
              {poses.map((output) => (
                <option key={identity(output)} value={identity(output)}>
                  {output.nodeId}.{output.port}
                </option>
              ))}
            </select>
          </label>
          <select
            aria-label="图像坐标系"
            value={frame}
            onChange={(event) => setFrame(event.target.value)}
          >
            <option value="">确认图像坐标系</option>
            {[
              ...new Set([
                ...poses.flatMap((output) =>
                  typeof output.value?.data?.frame === 'string' ? [output.value.data.frame] : [],
                ),
                ...cameras.flatMap((output) =>
                  typeof output.value?.data?.frame === 'string' ? [output.value.data.frame] : [],
                ),
              ]),
            ].map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
          <select
            aria-label="叠加实例身份"
            value={instance}
            onChange={(event) => setInstance(event.target.value)}
          >
            <option value="">全部实例</option>
            {(parsed.poses?.items ?? []).map((item) => (
              <option key={item.id} value={item.id}>
                {item.name || item.id} · {item.id}
              </option>
            ))}
          </select>
          <label>
            <input
              type="checkbox"
              checked={labels}
              onChange={(event) => setLabels(event.target.checked)}
            />
            标签
          </label>
          <label>
            <input
              type="checkbox"
              checked={axes}
              onChange={(event) => setAxes(event.target.checked)}
            />
            方向/轴
          </label>
          <label>
            掩膜
            <input
              aria-label="掩膜透明度"
              type="range"
              min="0"
              max="1"
              step="0.05"
              value={maskAlpha}
              onChange={(event) => setMaskAlpha(Number(event.target.value))}
            />
          </label>
          <label>
            线宽
            <input
              aria-label="工件叠加线宽"
              type="number"
              min="1"
              max="8"
              step="1"
              value={lineWidth}
              onChange={(event) =>
                setLineWidth(Math.max(1, Math.min(8, Number(event.target.value) || 1)))
              }
            />
          </label>
          <select
            aria-label="工件颜色分配"
            value={colorBy}
            onChange={(event) => {
              setColorBy(event.target.value as 'id' | 'class');
              setFixedColor('');
            }}
          >
            <option value="id">按身份着色</option>
            <option value="class">按类别着色</option>
          </select>
          <label>
            <input
              aria-label="固定工件颜色"
              type="color"
              value={fixedColor || '#5ec9d8'}
              onChange={(event) => setFixedColor(event.target.value)}
            />
            固定颜色
          </label>
          {(parsed.poses?.dimension === 3 || cloudGroups.length > 0) && (
            <>
              <select
                aria-label="投影相机"
                value={cameraPick}
                onChange={(event) => setCameraPick(event.target.value)}
              >
                <option value="">选择相机标定</option>
                {cameras.map((output) => (
                  <option key={identity(output)} value={identity(output)}>
                    {output.nodeId}.{output.port}
                  </option>
                ))}
              </select>
              <input
                aria-label="投影外参"
                placeholder={'跨帧外参 JSON：fromFrame/toFrame/matrix'}
                value={transformText}
                onChange={(event) => setTransformText(event.target.value)}
              />
            </>
          )}
          {cloudGroups.length > 0 && (
            <select
              aria-label="投影点云组"
              value={groupPick}
              onChange={(event) => {
                setGroupPick(event.target.value);
                setGroupId('');
                setGroupIndexPort('');
              }}
            >
              <option value="">不投影点云</option>
              {cloudGroups.map((output) => (
                <option key={identity(output)} value={identity(output)}>
                  {output.nodeId}.{output.port}
                </option>
              ))}
            </select>
          )}
          {group && (
            <>
              {related.outputs.filter(
                (output) => output.type === 'Indices' && output.nodeId === group.nodeId,
              ).length > 1 && (
                <select
                  aria-label="投影分组索引"
                  value={groupIndexPort}
                  onChange={(event) => setGroupIndexPort(event.target.value)}
                >
                  <option value="">选择配套 Indices</option>
                  {related.outputs
                    .filter((output) => output.type === 'Indices' && output.nodeId === group.nodeId)
                    .map((output) => (
                      <option key={output.port} value={output.port}>
                        {output.port}
                      </option>
                    ))}
                </select>
              )}
              <select
                aria-label="投影云组身份"
                value={groupId}
                onChange={(event) => setGroupId(event.target.value)}
              >
                <option value="">全部云组</option>
                {groupNames.map((item) => (
                  <option key={item.id} value={item.id}>
                    {item.id}
                  </option>
                ))}
              </select>
            </>
          )}
          <span role="status">
            {parsed.error ||
              render.error ||
              loadError ||
              related.error ||
              (group ? '点云投影上限 10 万点，按身份着色' : '')}
          </span>
        </div>
      </details>
    ) : null;
  return {
    toolbar,
    canvasRef,
    exportPng,
    overlay: (view: ImageOverlayView) => {
      if (!runId || !url || view.isCurrent === false) return null;
      const [left, top] = view.toStage(0, 0);
      return (
        <img
          src={url}
          alt="工件与点云投影"
          style={{
            position: 'absolute',
            left,
            top,
            width: width * view.scale,
            height: height * view.scale,
            pointerEvents: 'none',
          }}
        />
      );
    },
  };
}
