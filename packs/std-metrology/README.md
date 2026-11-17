# std-metrology

`std-metrology` 0.4.0 提供7项公共度量，默认随标准包启用。公开静态计算位于`lyflow::metrology_kernel`；算子负责坐标、来源、参数和诊断。不依赖PCL/OpenCV/ONNX Runtime，既有轮廓分析核仅在编译时使用固定Eigen头文件。

| 算子                          | 定义                                                     |
| ----------------------------- | -------------------------------------------------------- |
| `image.calibrated_distance`   | 平面标定后的物理距离，保持标定帧契约。                   |
| `profile.gap_flush`           | 既有裸云分析线的采样边缘间隙和同横坐标面差。             |
| `metrology.profile_gap_flush` | 原生Profile拟合统计的采样边缘间隙/右减左面差。           |
| `metrology.signed_point_line` | 同空间点线的有符号距离与垂足。                           |
| `metrology.flush_point_line`  | 同空间点线面差、补偿及显示几何。                         |
| `metrology.circle_gap`        | 圆心连线边界或基准方向切线间隙，保留左右角色和无效语义。 |
| `metrology.point_offset`      | 两轴位移分量及未补偿欧氏距离。                           |

各节点的测量定义不同，不能仅因都返回Measurement而互换。物理输入几何以米存储，毫米偏置及输出单位单独声明；节点按`profile.Geometry.v3`检查frame、plane、handedness，像素不能直接参与物理度量。零位移显示段可以输出，但作为输入几何时按退化线段拒绝。结果不附加业务合格判定。

完整接口、补偿顺序及非法输入变化见[公共Profile算子](../../docs/profile-public-operators.md)和[逐算子审计](../../docs/operator-package-audit.md)。独立静态SDK消费及依赖边界见[公共内核](../../docs/public-kernels.md)。
