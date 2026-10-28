# Shared visualization

`@lyflow/visualization` describes immutable output scenes and renders them as SVG. The editor uses the same `geometryPaths` function for algorithm geometry; the Node entry renders SVG through a local headless Edge/Chrome/Chromium process with GPU rendering disabled.

```js
import { renderPng } from '@lyflow/visualization/node';
const frame = { id: 'profile', unit: 'mm', axes: ['u', 'h'], yDirection: 'down' };
const result = await renderPng({
  schemaVersion: 1,
  frame,
  layers: [
    {
      id: 'output',
      frame,
      kind: 'points',
      points: [
        [0, 1],
        [2, 3],
      ],
    },
  ],
});
```

All layers declare frame, unit and projected axes. A layer in another frame requires an explicit affine transform `[a,b,c,d,e,f]`, mapping `(x,y)` to `(a*x+c*y+e,b*x+d*y+f)`. Bounds and geometry are never inferred by fitting. Nonfinite points are excluded and counted. Images must be embedded data URLs; the renderer has no network content access.

Geometry uses equal axis scales by default. Metric plots can declare `view.equalAspect: false` and separate `frame.axisUnits`. `annotations` show actual measurements without changing spatial bounds. `renderGridPng` shares bounds and aligns plot areas when requested; paginate grids rather than rendering more than 100 scenes at once. Returned metadata contains the actual rendered bounds, source references, point counts and annotations.

Set `LYFLOW_BROWSER` or pass `browserPath` if a supported browser is not installed in a standard location. Rendering has a 30-second default timeout and cleans up its temporary browser profile. The returned PNG, SVG and metadata can be persisted with the source snapshot.

The editor release entry `pnpm --filter @lyflow/editor pack:release --pack-destination <directory>` stages the shared source into the TypeScript tarball. Consumers do not need a separately published visualization package. Development continues to use the workspace package as the single source.

Run `pnpm --filter @lyflow/visualization test` for pure scene/coordinate/geometry checks. Actual PNG rendering should also be inspected on supported deployment platforms.
