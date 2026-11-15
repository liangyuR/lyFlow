# Python 类型适配示例

Python 3.11+、NumPy；使用随当前运行库一起交付的 `lyflow_python`。在 Python 节点管理中选择解释器并添加**本目录**后手动刷新，可发现五个算子。路径没有机器硬编码，输入不被修改，不创建文件、不依赖外部 XYZ Python 包。

| 算子 ID | 连线与约定 |
| --- | --- |
| `py.p6_adapters.offset_poses3d` | 接 `vision.Poses3D.v1` Record，米制偏移；local=false 在集合坐标系偏移，true 按工件 xyzw 四元数旋转偏移；保留 ID、姿态和属性 |
| `py.p6_adapters.filter_pickable` | 接标准 2D/3D 位姿集合，只有显式 `pickable=true` 的工件保留；缺字段不默认为可抓 |
| `py.p6_adapters.poses_to_json` | 位姿集合→`std.Value.v1` 字符串；保留类型版本、frame、unit、ID 和所有合法属性 |
| `py.p6_adapters.poses_from_json` | 上述字符串→原版本位姿集合；拒绝非法单位、四元数、重复 ID 和不支持的版本 |
| `py.p6_adapters.tensor_affine` | F32 Tensor→同 shape/scale Tensor，分块计算 gain*x+bias，并检查取消 |

最小流程：原生 `pose.construct_3d.pose`→`pose.concat.a`，集合输出 `poses` 连入偏移→可抓筛选→JSON 编码→JSON 解码。构造时应显式设置 `pickable=true` 以进入可抓筛选结果；缺少标记的工件不会被保留。空集合合法。使用米，不沿用旧脚本的隐含毫米；2D 位姿原有 px/m 原样保留。JSON 输出只用于小型数据和调试，受 SDK 字符串 64 KiB 限制；大量结果归档使用原生录制能力。

这五个节点独立实现旧脚本中可移植的数据操作，并提供 Tensor 示例。并非对原 `VisionPythonCustom`、protobuf、工业协议或机器人坐标接口的兼容层；日志、发布与回放使用原生能力，依赖 SKU、业务文件目录或硬件的脚本仍需单独迁移。完整分析见 `docs/p6-analysis-python.md`。

验证：在仓库根目录执行 `python -B -m unittest discover -s python/tests -v`。测试通过完整 worker 请求发现及执行本包，检查偏移、筛选、序列化、Tensor 与只读上游。
