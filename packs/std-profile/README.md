# std-profile

`std-profile` 0.3.0 提供原生 Profile 输入、适配、处理、选择及拟合。轻量配置含 14 项，不依赖 PCL/OpenCV/ONNX Runtime；显式启用公共稳健后端后注册另外两项拟合，共 16 项。完整端口和参数见[逐算子清单](../../docs/operator-package-audit/std-profile.md)。

| 算子                                                      | 行为                                                                 |
| --------------------------------------------------------- | -------------------------------------------------------------------- |
| `profile.from_cloud`                                      | 显式声明帧、单位、轴平面、手性和采样顺序；共享只读云，保留原槽。     |
| `profile.remap_axes`                                      | 明确指定轴排列和正负号，更新帧、平面和手性；不作为刚体变换解释。     |
| `profile.to_cloud`                                        | 明确换算 m/mm，导出裸云、完整 Profile 和小元数据；裸云无效槽置 NaN。 |
| `profile.input` / `profile.clean` / `profile.split_faces` | 显式原生输入、保槽/压缩清洗及分面；不猜轴与单位。                    |
| `profile.fit_line` / `profile.transform_pair`             | TLS拟合及明确坐标变换，保留原生来源身份。                            |
| `profile.analyze` / `profile.project_pair`                | 已迁入的既有裸云分析接口，保留原ID与契约。                           |
| `profile.bind_geometry`                                   | 明确外部米制点/线/圆/框的frame/plane和来源，不自动换轴。             |
| `profile.nearest_sample` / `profile.nearest_to_line`      | 按锚点距离或无限线垂距选有效原槽，保留物理源索引。                   |
| `profile.filter_validity`                                 | 显式保槽或压缩，所有通道与来源映射保持对应。                         |
| `profile.fit_line_robust` / `profile.fit_circle_robust`   | 可选PCL RANSAC/LM后端；与TLS方法区分。                               |

数据契约见[Profile](../../docs/profile-data-contract.md)，跨宿主接口见[协议](../../docs/profile-host-protocol.md)。M3已迁入`profile.analyze/project_pair`，`profile.gap_flush`归属`std-metrology`。M4b的v2几何来源、选槽和度量定义见[公共算子契约](../../docs/profile-public-operators.md)，旧Gap场景及导入图仍待M4c切换。

独立配置：

```powershell
python scripts/fetch-profile-math-deps.py --output-dir build/profile-math-deps
cmake -S core -B build/profile -G Ninja -DLYFLOW_STD_PACKS=OFF -DLYFLOW_PACKS=std-profile -DBUILD_TESTING=ON
cmake --build build/profile
ctest --test-dir build/profile --output-on-failure --no-tests=error
```

轻量分析只在库编译时使用固定Eigen头文件。需要两项稳健拟合时，另选`LYFLOW_MODULES=profile-robust-kernel`并提供PCL；该模块不隐式注册Gap场景或ONNX推理。算子库可用集合以实际manifest为准。

[随包示例](../../examples/m1-profile-adapters.lyflow.json) 用 1280 点合成云演示原生 Profile 流转、XZY 反射和 m→mm 换算；`sampleOrder=unknown`，不将随机合成云宣称为激光采样轮廓。`plane` 声明不要求第三轴为零。示例不进行 DTS 或 gap 测量。
