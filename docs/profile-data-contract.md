# Profile 数据契约

`Profile` 是带显式坐标语义和采样来源的原生轮廓数据。它使用共享只读 `PointCloud` 缓冲承载 XYZ 与可选通道，另存有效性和源槽索引。大数组不放入 `Record` 或 JSON。M1 提供契约、显式适配和宿主显示能力；DTS 场景算法迁移属于后续 M3。

## 描述字段

描述对象恰好包含以下 8 个字段，拒绝缺失字段、未知字段和未经声明的单位。

| 字段            | 约束                              | 含义                                                 |
| --------------- | --------------------------------- | ---------------------------------------------------- |
| `schemaVersion` | 整数 `1`                          | 契约版本。                                           |
| `frame`         | 非空 UTF-8 字符串，最多 1024 字节 | 当前坐标帧。                                         |
| `unit`          | `m` 或 `mm`                       | XYZ 坐标单位。                                       |
| `plane`         | `XY`、`XZ` 或 `YZ`                | 声明轮廓使用的轴平面，不要求第三轴为零，不自动旋转。 |
| `handedness`    | `right` 或 `reflected`            | 当前坐标系手性，不触发隐式反射。                     |
| `sampleOrder`   | `unknown` 或 `acquisition`        | 当前槽序是否仍为采集顺序。                           |
| `sourceId`      | 非空 UTF-8 字符串，最多 1024 字节 | 原始轮廓身份；与当前点云缓冲的 `cloudId` 分开。      |
| `sourceCount`   | JSON 整数，范围 `0..4294967295`   | 原始源槽总数，用于校验源索引。                       |

描述 JSON 上限为 8 KiB。`sourceCount` 使用数字，不使用十进制字符串。`frame` 与 `sourceId` 需要调用方明确提供，不由算子猜测。

## 槽、有效性和来源

| 数组            | 类型与形状     | 约束                                             |
| --------------- | -------------- | ------------------------------------------------ |
| XYZ             | `float32[N,3]` | 有效槽三个坐标均有限；任何槽都禁止正负无穷。     |
| intensity，可选 | `float32[N]`   | 长度与槽数一致。                                 |
| normals，可选   | `float32[N,3]` | 与 XYZ 同一坐标轴；单位换算不缩放法线。          |
| RGB，可选       | `uint8[N,3]`   | RGB 通道顺序。                                   |
| validity        | `uint8[N]`     | 每项严格为 `0` 或 `1`；`0` 可含 NaN 或有限坐标。 |
| sourceIndices   | `uint32[N]`    | 每项小于 `sourceCount`，整个数组无重复。         |

`acquisition` 要求 `sourceIndices` 严格递增；`unknown` 允许重排，仍要求唯一且不越界。源索引不要求连续。空轮廓允许 `N=0`，所有数组为空，仍须提供完整描述；`sourceCount` 可以为零或大于零。

例如原始轮廓有 1280 槽，第 120 槽未观测、第 121 槽被算法拒绝：保留 `N=1280`，将对应 `validity` 置零；第 120 槽可以是 NaN，第 121 槽可以保留有限的原始坐标。不得通过删除 NaN、排序 XYZ 或压缩数组来丢失槽位置。仅凭坐标是否有限，无法恢复算法拒绝标记。

原生输入共享只读点云；改变坐标或通道必须分配新缓冲。Python worker 输入数组以只读 NumPy memmap 提供，描述对象也不可变。节点调用 `.copy()` 得到所有数组的独立可写副本，再修改有效采样。数组维度、通道长度、掩码和来源索引在输入及输出边界校验；非法输出不会覆盖上游数据。

## 显式适配

| 算子                 | 行为                                                                                                                                                                                                                            |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `profile.from_cloud` | 明确声明原始坐标单位、帧、平面、手性和来源身份；共享点云，不换轴、不排序、不抽稀。有限 XYZ 标为有效，NaN 槽保留并标为无效，源索引初始化为 `0..N-1`。选择 `acquisition` 表示调用方确认当前点序就是采集序。                       |
| `profile.to_cloud`   | 只按目标 `m/mm` 换算 XYZ，保持轴和点序，法线不缩放；裸云无效槽全部置 NaN，避免被当作观测。另输出完整 `Profile`，保留原始无效载荷、有效性和源索引，以及仅含小元数据的描述 `Record`。裸云与描述 Record 两者无法独立恢复完整轮廓。 |
| `profile.remap_axes` | 按明确的轴排列和正负号映射 XYZ 与法线，更新目标帧、平面及手性；保持单位、有效性、槽序和来源身份。反射不能伪装成右手刚体 `Transform`。                                                                                           |

`profile.from_cloud` 的单位是对输入数值的声明，不是自动换算。常规米制点云应声明 `m`；确认来自旧毫米数据时才声明 `mm`。向按米解释 `PointCloud` 的标准算子导出时，应选择 `profile.to_cloud` 的默认目标 `m`。旧 DTS 几何须单独通过声明旧契约的 `dts.geometry_to_metric` 适配，不能因旧 `Meter` 标签而认定数值已经是米，也不能重复换算。

## Python 与临时文件协议

SDK 构造接口：

```python
Profile(xyz, descriptor, validity, source_indices,
        intensity=None, normals=None, rgb=None)
```

`Profile` 独立于 `PointCloud`，不会通过继承在输出编码时丢失描述。节点声明 `inputs={"profile": "Profile"}` 与 `outputs={"profile": "Profile"}`。Python 属性使用 `source_indices`；协议键使用 `sourceIndices`。

worker DTO 的结构如下，数组描述中的 `file` 是本请求临时目录内的绝对路径：

```json
{
  "type": "Profile",
  "descriptor": {
    "schemaVersion": 1,
    "frame": "scanner",
    "unit": "mm",
    "plane": "XZ",
    "handedness": "right",
    "sampleOrder": "acquisition",
    "sourceId": "scan-1",
    "sourceCount": 1280
  },
  "xyz": { "file": "<requestDir>/xyz.bin", "shape": [1280, 3], "dtype": "f32" },
  "validity": { "file": "<requestDir>/validity.bin", "shape": [1280], "dtype": "u8" },
  "sourceIndices": { "file": "<requestDir>/source-indices.bin", "shape": [1280], "dtype": "u32" }
}
```

可选 `intensity`、`normals`、`rgb` 使用同样的数组描述，类型与前表一致。二进制数值采用小端，`u32` 不得替换为浮点索引或 `u8`。JSON 仅保存小描述，大数组通过请求专用二进制文件和 NumPy memmap 交换。输出复制到引擎自有数据后清理文件；异常、取消和超时也必须清理。不得把输入 memmap 保存到下一次调用。worker 的协议版本、取消和超时约束见 [Python 自定义节点](python-nodes.md)。

可直接运行 [profile_copy.py](../python/examples/profile_copy.py)：

```powershell
$env:PYTHONPATH = (Resolve-Path -LiteralPath ./python).Path
python -B ./python/examples/profile_copy.py
```

在已安装 SDK 和 NumPy 的 Python 3.11+ 环境中运行即可。示例仅演示有效槽的 Z 偏移，偏移参数按米声明并依据 Profile 单位显式换算；验证 1280 槽、NaN 与有限无效槽保留、源索引及描述不变、原输入未修改。也可在 Python 节点管理中直接添加该脚本。它不实现 DTS 场景检测算法。

## 宿主二进制与编辑器显示

宿主通过可选 `getOutputProfile(runId, nodeId, port)` 提供独立 `LYPR` 载荷，不退回普通点云接口来猜测单位，也不按预览点数抽稀。缺少该能力时，编辑器提示升级宿主。

`LYPR` v1 头为 8 个小端 uint32：magic `0x5250594c`、版本、当前槽数、总槽数、起始槽偏移、通道 flags、元数据字节数、保留零。后接 UTF-8 元数据 `{descriptor, cloudId}` 和四字节对齐的 XYZ、可选 intensity/normals/RGB、validity、sourceIndices。`cloudId` 是当前 carrier 的 uint64 十进制字符串，不是 `sourceId`。M1 的编辑器请求全量槽；解码校验字节数、描述、坐标、掩码、索引与顺序。

编辑器保留原始数组，额外分配私有 XYZ 显示缓冲：仅在此缓冲把 `mm` 乘 `0.001` 转成米，并将无效槽的三个坐标都设为 NaN。不修改原始数据，不压缩槽，不按 `plane` 旋转，不按 `handedness` 隐式反射。显示边界只统计有效槽。界面同时展示源单位、平面、手性、采样顺序和坐标帧，避免把毫米数据直接当作米。

Profile 独立查看默认使用三维视图，提供三维和值视图；不使用固定 XY 二维相机来隐式表达 XZ/YZ 轮廓。通用主预览仍可由用户切换视图。此处验证的是数据契约和软件显示，不代表扫描器采样、DTS 现场质量或工业节拍已完成验收。
