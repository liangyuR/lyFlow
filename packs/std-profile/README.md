# 轮廓数据适配包

`std-profile` 在 M1 提供三项显式适配，不依赖 PCL、OpenCV 或 ONNX Runtime。

| 算子                 | 行为                                                                 |
| -------------------- | -------------------------------------------------------------------- |
| `profile.from_cloud` | 显式声明帧、单位、轴平面、手性和采样顺序；共享只读云，保留原槽。     |
| `profile.remap_axes` | 明确指定轴排列和正负号，更新帧、平面和手性；不作为刚体变换解释。     |
| `profile.to_cloud`   | 明确换算 m/mm，导出裸云、完整 Profile 和小元数据；裸云无效槽置 NaN。 |

数据契约见 [Profile](../../docs/profile-data-contract.md)，跨宿主接口见[协议](../../docs/profile-host-protocol.md)。原有 `profile.analyze/gap_flush/project_pair` 目前仍在 `std-pointcloud`，不因前缀相同自动迁移。清洗、分段、拟合的迁移属于后续 M3。

独立配置：

```powershell
cmake -S core -B build/profile -G Ninja -DLYFLOW_STD_PACKS=OFF -DLYFLOW_PACKS=std-profile -DBUILD_TESTING=ON
cmake --build build/profile
ctest --test-dir build/profile --output-on-failure --no-tests=error
```

[随包示例](../../examples/m1-profile-adapters.lyflow.json) 用 1280 点合成云演示原生 Profile 流转、XZY 反射和 m→mm 换算；`sampleOrder=unknown`，不将随机合成云宣称为激光采样轮廓。`plane` 声明不要求第三轴为零。示例不进行 DTS 或 gap 测量。
