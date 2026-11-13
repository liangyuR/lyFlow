import numpy as np

from lyflow_python import PointCloud, node


@node(
    id="py.example.offset", version="1.0.0", label="Python 点云平移",
    inputs={"cloud": "PointCloud"}, outputs={"cloud": "PointCloud"},
    params={"offset": {"type": "float", "default": 0.01, "unit": "m", "label": "Z 平移"}},
)
def offset(inputs, params, ctx):
    cloud = inputs["cloud"]
    ctx.log(f"处理 {len(cloud.xyz)} 个点，线程预算 {ctx.thread_budget}")
    result = cloud.copy()
    chunk = 100_000
    for start in range(0, len(result.xyz), chunk):
        ctx.check_cancelled()
        result.xyz[start:start + chunk, 2] += np.float32(params["offset"])
        ctx.progress(min(1.0, (start + chunk) / max(1, len(result.xyz))))
    return {"cloud": PointCloud(result.xyz, result.intensity, result.normals, result.rgb)}
